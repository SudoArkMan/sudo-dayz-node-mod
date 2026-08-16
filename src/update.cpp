#include "update.h"

#include "version.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QTimer>

#include <limits>

namespace {

constexpr int kDefaultTimeoutMs = 10000;
// Once a day. The check is a courtesy, and GitHub allows an unauthenticated
// address sixty requests an hour across everything it runs.
constexpr qint64 kCheckIntervalSecs = 24 * 60 * 60;

const QString kKeyConsent = QStringLiteral("updates/consent");
const QString kKeyOwner = QStringLiteral("updates/owner");
const QString kKeyRepository = QStringLiteral("updates/repository");
const QString kKeyLastChecked = QStringLiteral("updates/lastChecked");
const QString kKeySkipped = QStringLiteral("updates/skippedVersion");
const QString kKeyLastSeen = QStringLiteral("updates/lastSeenVersion");
const QString kKeyPrereleases = QStringLiteral("updates/includePrereleases");

bool isAsciiDigits(const QString &text)
{
    if (text.isEmpty()) return false;
    for (const QChar c : text)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return false;
    return true;
}

// Numeric identifiers compare as numbers however long they are, so a
// pre-release counter that has run past what an int holds still orders. Leading
// zeros are not legal semver but they turn up, and dropping them first is what
// makes 007 and 7 the same number rather than two spellings that sort apart.
int compareNumericIdentifier(const QString &a, const QString &b)
{
    QString left = a;
    QString right = b;
    while (left.size() > 1 && left.startsWith(QLatin1Char('0'))) left.remove(0, 1);
    while (right.size() > 1 && right.startsWith(QLatin1Char('0'))) right.remove(0, 1);
    if (left.size() != right.size()) return left.size() < right.size() ? -1 : 1;
    const int cmp = QString::compare(left, right);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

// semver.org, rule 11.4: numeric identifiers order below alphanumeric ones, and
// alphanumeric ones order by ASCII.
int comparePreIdentifier(const QString &a, const QString &b)
{
    const bool leftNumeric = isAsciiDigits(a);
    const bool rightNumeric = isAsciiDigits(b);
    if (leftNumeric && rightNumeric) return compareNumericIdentifier(a, b);
    if (leftNumeric != rightNumeric) return leftNumeric ? -1 : 1;
    const int cmp = QString::compare(a, b, Qt::CaseSensitive);
    return cmp < 0 ? -1 : (cmp > 0 ? 1 : 0);
}

// `[0.2.0]`, `[0.2.0](https://example.invalid/tag)` and a bare `0.2.0` are all
// the same token wearing different amounts of markdown.
QString unwrapHeadingToken(const QString &text)
{
    QString token = text.trimmed();
    static const QRegularExpression link(
        QStringLiteral("^\\[([^\\]]*)\\]\\s*(\\([^\\)]*\\))?"));
    const QRegularExpressionMatch match = link.match(token);
    if (match.hasMatch()) token = match.captured(1).trimmed();
    return token;
}

// The markers a panel should not be drawing. Links keep their text, inline code
// and bold lose their fences, and an image is dropped whole because its target
// is the only part of it that carried anything.
QString stripMarkdown(const QString &text)
{
    QString out = text;
    static const QRegularExpression image(QStringLiteral("!\\[[^\\]]*\\]\\([^\\)]*\\)"));
    static const QRegularExpression link(
        QStringLiteral("\\[([^\\]]*)\\]\\(([^\\)]*)\\)"));
    static const QRegularExpression code(QStringLiteral("`([^`]*)`"));
    static const QRegularExpression bold(QStringLiteral("\\*\\*([^*]*)\\*\\*"));
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    out.replace(image, QString());
    out.replace(link, QStringLiteral("\\1"));
    out.replace(code, QStringLiteral("\\1"));
    out.replace(bold, QStringLiteral("\\1"));
    out.replace(spaces, QStringLiteral(" "));
    return out.trimmed();
}

// The asset a Windows user is after. An installer first, then anything else
// that installs, then an archive. A release with nothing but source tarballs
// leaves this empty and the page falls back to the release page itself.
int assetRank(const QString &name)
{
    const QString lower = name.toLower();
    if (lower.endsWith(QStringLiteral(".exe"))) {
        return lower.contains(QStringLiteral("setup"))
                       || lower.contains(QStringLiteral("install"))
                   ? 0
                   : 1;
    }
    if (lower.endsWith(QStringLiteral(".msi"))) return 2;
    if (lower.endsWith(QStringLiteral(".zip"))) return 3;
    if (lower.endsWith(QStringLiteral(".7z"))) return 4;
    return -1;
}

Release readRelease(const QJsonObject &object)
{
    Release release;
    release.tag = object.value(QStringLiteral("tag_name")).toString().trimmed();
    if (release.tag.isEmpty())
        release.tag = object.value(QStringLiteral("name")).toString().trimmed();
    release.version = parseVersion(release.tag);
    release.title = object.value(QStringLiteral("name")).toString().trimmed();
    if (release.title.isEmpty()) release.title = release.tag;
    release.notes = object.value(QStringLiteral("body")).toString();
    release.pageUrl = object.value(QStringLiteral("html_url")).toString();
    release.draft = object.value(QStringLiteral("draft")).toBool();
    release.prerelease = object.value(QStringLiteral("prerelease")).toBool()
                         || release.version.isPrerelease();
    release.published = QDateTime::fromString(
        object.value(QStringLiteral("published_at")).toString(), Qt::ISODate);

    int best = std::numeric_limits<int>::max();
    const QJsonArray assets = object.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue &value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        const int rank = assetRank(name);
        if (rank < 0 || rank >= best) continue;
        best = rank;
        release.assetName = name;
        release.assetUrl =
            asset.value(QStringLiteral("browser_download_url")).toString();
        release.assetSize =
            qint64(asset.value(QStringLiteral("size")).toDouble());
    }
    return release;
}

} // namespace

// ---------------------------------------------------------------------------
// Versions
// ---------------------------------------------------------------------------

QString Version::toString() const
{
    if (!valid) return {};
    QString text = QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
    if (!pre.isEmpty()) text += QLatin1Char('-') + pre.join(QLatin1Char('.'));
    if (!build.isEmpty()) text += QLatin1Char('+') + build;
    return text;
}

Version parseVersion(const QString &text)
{
    Version version;
    QString body = text.trimmed();
    if (body.isEmpty()) return version;
    if (body.startsWith(QLatin1Char('v')) || body.startsWith(QLatin1Char('V')))
        body.remove(0, 1);
    if (body.isEmpty()) return version;

    const int plus = body.indexOf(QLatin1Char('+'));
    if (plus >= 0) {
        version.build = body.mid(plus + 1);
        body = body.left(plus);
        if (version.build.isEmpty()) return version;
    }

    QString pre;
    const int dash = body.indexOf(QLatin1Char('-'));
    if (dash >= 0) {
        pre = body.mid(dash + 1);
        body = body.left(dash);
        if (pre.isEmpty()) return version;
    }

    const QStringList parts = body.split(QLatin1Char('.'));
    if (parts.isEmpty() || parts.size() > 3) return version;
    int numbers[3] = {0, 0, 0};
    for (int i = 0; i < parts.size(); ++i) {
        const QString part = parts.at(i);
        if (!isAsciiDigits(part)) return version;
        bool ok = false;
        const qlonglong value = part.toLongLong(&ok);
        if (!ok || value > std::numeric_limits<int>::max()) return version;
        numbers[i] = int(value);
    }
    version.major = numbers[0];
    version.minor = numbers[1];
    version.patch = numbers[2];

    if (dash >= 0) {
        version.pre = pre.split(QLatin1Char('.'));
        for (const QString &identifier : version.pre)
            if (identifier.isEmpty()) return Version();
    }

    version.valid = true;
    return version;
}

int compareVersions(const Version &a, const Version &b)
{
    if (!a.valid || !b.valid) {
        if (a.valid == b.valid) return 0;
        return a.valid ? 1 : -1;
    }
    if (a.major != b.major) return a.major < b.major ? -1 : 1;
    if (a.minor != b.minor) return a.minor < b.minor ? -1 : 1;
    if (a.patch != b.patch) return a.patch < b.patch ? -1 : 1;
    // A release outranks any pre-release of the same three numbers, which is
    // the half of the rule that string comparison gets backwards.
    if (a.pre.isEmpty() != b.pre.isEmpty()) return a.pre.isEmpty() ? 1 : -1;

    const int shared = qMin(a.pre.size(), b.pre.size());
    for (int i = 0; i < shared; ++i) {
        const int cmp = comparePreIdentifier(a.pre.at(i), b.pre.at(i));
        if (cmp != 0) return cmp;
    }
    if (a.pre.size() != b.pre.size()) return a.pre.size() < b.pre.size() ? -1 : 1;
    return 0;
}

int compareVersions(const QString &a, const QString &b)
{
    return compareVersions(parseVersion(a), parseVersion(b));
}

bool isNewerVersion(const QString &candidate, const QString &current)
{
    const Version left = parseVersion(candidate);
    const Version right = parseVersion(current);
    if (!left.valid || !right.valid) return false;
    return compareVersions(left, right) > 0;
}

// ---------------------------------------------------------------------------
// The changelog
// ---------------------------------------------------------------------------

QVector<ChangelogEntry> parseChangelog(const QString &text)
{
    QVector<ChangelogEntry> entries;
    if (text.isEmpty()) return entries;

    static const QRegularExpression newline(QStringLiteral("\r\n|\n|\r"));
    static const QRegularExpression separator(
        QStringLiteral("\\s+[-\\x{2013}\\x{2014}]\\s+"));
    const QStringList lines = text.split(newline);

    bool fenced = false;
    int current = -1;
    QStringList body;

    const auto close = [&entries, &current, &body]() {
        if (current >= 0) entries[current].body = body.join(QLatin1Char('\n')).trimmed();
        body.clear();
    };

    for (const QString &raw : lines) {
        const QString trimmed = raw.trimmed();
        if (trimmed.startsWith(QStringLiteral("```"))
            || trimmed.startsWith(QStringLiteral("~~~"))) {
            fenced = !fenced;
            if (current >= 0) body.append(raw);
            continue;
        }
        // A heading inside a fence is a line of somebody's example, not a
        // release. Reading it as one is how a changelog with a sample file in
        // it grows versions nobody shipped.
        if (!fenced && trimmed.startsWith(QStringLiteral("## "))) {
            close();
            current = -1;

            QString rest = trimmed.mid(3).trimmed();
            ChangelogEntry entry;
            if (rest.contains(QStringLiteral("[YANKED]"), Qt::CaseInsensitive)) {
                entry.yanked = true;
                rest.remove(QStringLiteral("[YANKED]"), Qt::CaseInsensitive);
                rest = rest.trimmed();
            }

            QString token = rest;
            QString datePart;
            const QRegularExpressionMatch split = separator.match(rest);
            if (split.hasMatch()) {
                token = rest.left(split.capturedStart());
                datePart = rest.mid(split.capturedEnd()).trimmed();
            }
            token = unwrapHeadingToken(token);
            if (token.isEmpty()) continue;

            if (token.compare(QStringLiteral("unreleased"), Qt::CaseInsensitive) == 0) {
                entry.unreleased = true;
                entry.heading = QStringLiteral("Unreleased");
            } else {
                entry.version = parseVersion(token);
                if (!entry.version.valid) continue;
                entry.heading = token;
            }
            if (!datePart.isEmpty()) {
                entry.date = QDate::fromString(datePart.left(10), Qt::ISODate);
                if (!entry.date.isValid())
                    entry.date = QDate::fromString(datePart, QStringLiteral("d MMM yyyy"));
            }
            entries.append(entry);
            current = entries.size() - 1;
            continue;
        }
        if (current >= 0) body.append(raw);
    }
    close();
    return entries;
}

ChangelogEntry changelogEntryFor(const QVector<ChangelogEntry> &entries,
                                 const QString &version)
{
    const Version wanted = parseVersion(version);
    if (!wanted.valid) return {};
    for (const ChangelogEntry &entry : entries) {
        if (entry.unreleased || !entry.version.valid) continue;
        if (compareVersions(entry.version, wanted) == 0) return entry;
    }
    return {};
}

QStringList changelogLines(const QString &body, int maxLines)
{
    QStringList out;
    if (body.isEmpty()) return out;

    static const QRegularExpression newline(QStringLiteral("\r\n|\n|\r"));
    static const QRegularExpression marker(QStringLiteral("^([-*+]|\\d+\\.)\\s+"));
    bool fenced = false;
    bool inItem = false;

    for (const QString &raw : body.split(newline)) {
        QString line = raw.trimmed();
        if (line.startsWith(QStringLiteral("```"))
            || line.startsWith(QStringLiteral("~~~"))) {
            fenced = !fenced;
            inItem = false;
            continue;
        }
        if (fenced) continue;
        if (line.isEmpty()) {
            inItem = false;
            continue;
        }

        if (line.startsWith(QLatin1Char('#'))) {
            while (line.startsWith(QLatin1Char('#'))) line.remove(0, 1);
            line = stripMarkdown(line);
            inItem = false;
            if (line.isEmpty()) continue;
            // A section name is a label for what follows, so it reads as one.
            if (!line.endsWith(QLatin1Char(':'))) line += QLatin1Char(':');
        } else {
            const bool starts = marker.match(line).hasMatch();
            // An item wrapped over two lines in the file is one item. Markdown
            // says so, and a panel that draws the wrap as a second entry turns
            // a three item release into a six line one.
            const bool continues = !starts && inItem && !out.isEmpty();
            line.remove(marker);
            line = stripMarkdown(line);
            if (line.isEmpty()) continue;
            if (continues) {
                out.last() += QLatin1Char(' ') + line;
                continue;
            }
            inItem = true;
        }

        if (maxLines > 0 && out.size() >= maxLines) break;
        out.append(line);
    }

    // A cap that lands on a section label leaves a heading with nothing under
    // it, which reads as a line lost rather than a line saved.
    if (maxLines > 0 && !out.isEmpty() && out.last().endsWith(QLatin1Char(':')))
        out.removeLast();
    return out;
}

QString findChangelog(const QString &startDir)
{
    const QString base = startDir.isEmpty() ? QCoreApplication::applicationDirPath()
                                            : startDir;
    if (base.isEmpty()) return {};
    QDir dir(base);
    for (int level = 0; level < 5; ++level) {
        const QString candidate = dir.absoluteFilePath(QStringLiteral("CHANGELOG.md"));
        if (QFileInfo(candidate).isFile()) return QDir::cleanPath(candidate);
        if (!dir.cdUp()) break;
    }
    return {};
}

QString readChangelog(const QString &path, QString *error)
{
    if (error) error->clear();
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("No changelog was given.");
        return {};
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return {};
    }
    // Read as bytes and decode once. A QTextStream would pick an encoding from
    // the build's Qt version, and a changelog is UTF-8 by convention wherever
    // Keep a Changelog is followed.
    return QString::fromUtf8(file.readAll());
}

// ---------------------------------------------------------------------------
// Releases
// ---------------------------------------------------------------------------

QVector<Release> parseReleases(const QByteArray &json, QString *error)
{
    if (error) error->clear();
    QVector<Release> releases;
    if (json.trimmed().isEmpty()) {
        if (error) *error = QStringLiteral("The reply was empty.");
        return releases;
    }

    QJsonParseError parse{};
    const QJsonDocument document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError) {
        if (error)
            *error = QStringLiteral("The reply was not readable as JSON (%1).")
                         .arg(parse.errorString());
        return releases;
    }
    if (!document.isArray()) {
        if (error) *error = QStringLiteral("The reply was not a list of releases.");
        return releases;
    }

    for (const QJsonValue &value : document.array()) {
        if (!value.isObject()) continue;
        const Release release = readRelease(value.toObject());
        // A tag nothing can order is a tag nothing should be compared against.
        if (!release.version.valid) continue;
        releases.append(release);
    }
    return releases;
}

Release newestRelease(const QVector<Release> &releases, const QString &currentVersion,
                      bool includePrereleases)
{
    Release best;
    for (const Release &release : releases) {
        if (release.draft) continue;
        if (release.prerelease && !includePrereleases) continue;
        if (!isNewerVersion(release.tag, currentVersion)) continue;
        if (best.isValid() && compareVersions(release.version, best.version) <= 0)
            continue;
        best = release;
    }
    return best;
}

// ---------------------------------------------------------------------------
// The decision
// ---------------------------------------------------------------------------

UpdateOutcome evaluateFetch(const FetchOutcome &fetch, const QString &currentVersion,
                            bool includePrereleases, const QString &skipVersion)
{
    UpdateOutcome outcome;
    outcome.checkedAt = fetch.at.isValid() ? fetch.at : QDateTime::currentDateTimeUtc();

    // Nothing came back at all: no network, a name that does not resolve, or
    // the timeout. All three are the same thing to the user, which is nothing.
    if (!fetch.completed) {
        outcome.status = UpdateOutcome::Failed;
        outcome.detail = fetch.transportError.isEmpty()
                             ? QStringLiteral("The request did not complete.")
                             : fetch.transportError;
        return outcome;
    }

    const int code = fetch.httpStatus;
    if (code == 404) {
        outcome.status = UpdateOutcome::Failed;
        outcome.detail = QStringLiteral("That repository has no releases to read.");
        return outcome;
    }
    if (code == 403 || code == 429) {
        outcome.status = UpdateOutcome::Failed;
        outcome.detail = QStringLiteral("GitHub is rate limiting this address. It "
                                        "will answer again within the hour.");
        return outcome;
    }
    if (code < 200 || code >= 300) {
        outcome.status = UpdateOutcome::Failed;
        outcome.detail = code > 0
                             ? QStringLiteral("GitHub answered with %1.").arg(code)
                             : QStringLiteral("The reply carried no status.");
        return outcome;
    }

    QString error;
    const QVector<Release> releases = parseReleases(fetch.body, &error);
    if (!error.isEmpty()) {
        outcome.status = UpdateOutcome::Failed;
        outcome.detail = error;
        return outcome;
    }
    if (releases.isEmpty()) {
        outcome.status = UpdateOutcome::NoReleases;
        outcome.detail = QStringLiteral("No published release carries a version this "
                                        "app can read.");
        return outcome;
    }

    const Release newer = newestRelease(releases, currentVersion, includePrereleases);
    if (!newer.isValid()) {
        outcome.status = UpdateOutcome::UpToDate;
        outcome.detail = QStringLiteral("Nothing published is newer than %1.")
                             .arg(currentVersion);
        return outcome;
    }

    // A version the user turned down stays turned down, and so does everything
    // older than it. Saying no once should not mean saying no every day.
    if (!skipVersion.isEmpty()
        && compareVersions(newer.version, parseVersion(skipVersion)) <= 0) {
        outcome.status = UpdateOutcome::UpToDate;
        outcome.detail = QStringLiteral("%1 was skipped.").arg(newer.version.toString());
        return outcome;
    }

    outcome.status = UpdateOutcome::UpdateAvailable;
    outcome.release = newer;
    outcome.detail = QStringLiteral("%1 is newer than %2.")
                         .arg(newer.version.toString(), currentVersion);
    return outcome;
}

// ---------------------------------------------------------------------------
// What the user has agreed to
// ---------------------------------------------------------------------------

// Both come out of version.h, which CMake configures from cache entries. The
// repository does not exist yet, which is exactly why these are configuration
// and not two strings in the middle of a URL.
QString defaultUpdateOwner()
{
    return QString::fromLatin1(nodemod::kUpdateOwner);
}

QString defaultUpdateRepository()
{
    return QString::fromLatin1(nodemod::kUpdateRepository);
}

UpdatePreferences::UpdatePreferences(const QString &iniPath)
    : m_settings(iniPath.isEmpty() ? new QSettings()
                                   : new QSettings(iniPath, QSettings::IniFormat))
{
}

UpdatePreferences::~UpdatePreferences()
{
    if (m_settings) m_settings->sync();
    delete m_settings;
}

UpdateConsent UpdatePreferences::consent() const
{
    const QString value = m_settings->value(kKeyConsent).toString();
    if (value.compare(QStringLiteral("allowed"), Qt::CaseInsensitive) == 0)
        return UpdateConsent::Allowed;
    if (value.compare(QStringLiteral("declined"), Qt::CaseInsensitive) == 0)
        return UpdateConsent::Declined;
    return UpdateConsent::NotAsked;
}

void UpdatePreferences::setConsent(UpdateConsent consent)
{
    switch (consent) {
    case UpdateConsent::Allowed:
        m_settings->setValue(kKeyConsent, QStringLiteral("allowed"));
        break;
    case UpdateConsent::Declined:
        m_settings->setValue(kKeyConsent, QStringLiteral("declined"));
        break;
    case UpdateConsent::NotAsked:
        m_settings->remove(kKeyConsent);
        break;
    }
    m_settings->sync();
}

QString UpdatePreferences::owner() const
{
    const QString value = m_settings->value(kKeyOwner).toString().trimmed();
    return value.isEmpty() ? defaultUpdateOwner() : value;
}

QString UpdatePreferences::repository() const
{
    const QString value = m_settings->value(kKeyRepository).toString().trimmed();
    return value.isEmpty() ? defaultUpdateRepository() : value;
}

void UpdatePreferences::setRepository(const QString &owner, const QString &repository)
{
    m_settings->setValue(kKeyOwner, owner.trimmed());
    m_settings->setValue(kKeyRepository, repository.trimmed());
    m_settings->sync();
}

QDateTime UpdatePreferences::lastChecked() const
{
    return QDateTime::fromString(m_settings->value(kKeyLastChecked).toString(),
                                 Qt::ISODate);
}

void UpdatePreferences::setLastChecked(const QDateTime &when)
{
    if (when.isValid())
        m_settings->setValue(kKeyLastChecked, when.toUTC().toString(Qt::ISODate));
    else
        m_settings->remove(kKeyLastChecked);
    m_settings->sync();
}

QString UpdatePreferences::skippedVersion() const
{
    return m_settings->value(kKeySkipped).toString();
}

void UpdatePreferences::setSkippedVersion(const QString &version)
{
    if (version.isEmpty()) m_settings->remove(kKeySkipped);
    else m_settings->setValue(kKeySkipped, version);
    m_settings->sync();
}

QString UpdatePreferences::lastSeenVersion() const
{
    return m_settings->value(kKeyLastSeen).toString();
}

void UpdatePreferences::setLastSeenVersion(const QString &version)
{
    m_settings->setValue(kKeyLastSeen, version);
    m_settings->sync();
}

bool UpdatePreferences::includePrereleases() const
{
    return m_settings->value(kKeyPrereleases, false).toBool();
}

void UpdatePreferences::setIncludePrereleases(bool on)
{
    m_settings->setValue(kKeyPrereleases, on);
    m_settings->sync();
}

bool UpdatePreferences::dueForCheck(const QDateTime &now) const
{
    if (consent() != UpdateConsent::Allowed) return false;
    const QDateTime last = lastChecked();
    if (!last.isValid()) return true;
    // A clock that has been put back leaves a stamp in the future. Treating it
    // as due is better than never checking again on that machine.
    const qint64 elapsed = last.secsTo(now);
    return elapsed < 0 || elapsed >= kCheckIntervalSecs;
}

void UpdatePreferences::flush()
{
    if (m_settings) m_settings->sync();
}

// ---------------------------------------------------------------------------
// The request itself
// ---------------------------------------------------------------------------

struct UpdateCheck::Private {
    QNetworkAccessManager *network = nullptr;
    QNetworkReply *reply = nullptr;
    QTimer *timer = nullptr;
    QString owner = defaultUpdateOwner();
    QString repository = defaultUpdateRepository();
    QString currentVersion;
    QString skipVersion;
    bool includePrereleases = false;
    int timeout = kDefaultTimeoutMs;
};

UpdateCheck::UpdateCheck(QObject *parent)
    : QObject(parent), d(new Private)
{
    d->currentVersion = QCoreApplication::applicationVersion();
    d->timer = new QTimer(this);
    d->timer->setSingleShot(true);
    // Belt and braces with the request's own transfer timeout: a reply that
    // trickles a byte a second resets that one and never finishes.
    connect(d->timer, &QTimer::timeout, this, [this]() {
        if (!d->reply) return;
        QNetworkReply *reply = d->reply;
        d->reply = nullptr;
        reply->disconnect(this);
        reply->abort();
        reply->deleteLater();
        FetchOutcome fetch;
        fetch.at = QDateTime::currentDateTimeUtc();
        fetch.transportError = QStringLiteral("The check took too long and was "
                                              "given up on.");
        report(fetch);
    });
}

UpdateCheck::~UpdateCheck()
{
    cancel();
    delete d;
}

void UpdateCheck::setRepository(const QString &owner, const QString &repository)
{
    if (!owner.trimmed().isEmpty()) d->owner = owner.trimmed();
    if (!repository.trimmed().isEmpty()) d->repository = repository.trimmed();
}

void UpdateCheck::setCurrentVersion(const QString &version)
{
    d->currentVersion = version;
}

void UpdateCheck::setIncludePrereleases(bool on) { d->includePrereleases = on; }

void UpdateCheck::setSkippedVersion(const QString &version)
{
    d->skipVersion = version;
}

void UpdateCheck::setTimeout(int milliseconds)
{
    d->timeout = qMax(1000, milliseconds);
}

QUrl UpdateCheck::endpoint() const
{
    return QUrl(QStringLiteral("https://api.github.com/repos/%1/%2/releases?per_page=20")
                    .arg(QString::fromUtf8(QUrl::toPercentEncoding(d->owner)),
                         QString::fromUtf8(QUrl::toPercentEncoding(d->repository))));
}

bool UpdateCheck::running() const { return d->reply != nullptr; }

void UpdateCheck::start()
{
    if (d->reply) return;
    if (!d->network) d->network = new QNetworkAccessManager(this);

    QNetworkRequest request(endpoint());
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("SUDO-DayZ-Node-Mod/%1")
                          .arg(d->currentVersion.isEmpty()
                                   ? QStringLiteral("0")
                                   : d->currentVersion));
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(d->timeout);

    d->reply = d->network->get(request);
    d->timer->start(d->timeout + 2000);

    connect(d->reply, &QNetworkReply::finished, this, [this]() {
        QNetworkReply *reply = d->reply;
        if (!reply) return;
        d->reply = nullptr;
        d->timer->stop();

        FetchOutcome fetch;
        fetch.at = QDateTime::currentDateTimeUtc();
        fetch.httpStatus =
            reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError)
            fetch.transportError = reply->errorString();
        // A status means the far end answered, whatever it thought of the
        // request. Only a reply that never got one counts as not completed.
        if (fetch.httpStatus > 0) {
            fetch.completed = true;
            fetch.body = reply->readAll();
        }
        reply->deleteLater();
        report(fetch);
    });
}

void UpdateCheck::cancel()
{
    d->timer->stop();
    if (!d->reply) return;
    QNetworkReply *reply = d->reply;
    d->reply = nullptr;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
    // Nothing is emitted: the caller asked for this one to stop, and a result
    // it did not ask for is exactly what the whole feature is trying to avoid.
}

void UpdateCheck::report(const FetchOutcome &fetch)
{
    emit finished(evaluateFetch(fetch, d->currentVersion, d->includePrereleases,
                                d->skipVersion));
}
