#include "modlibrary.h"

#include "builtins.h"
#include "catalog.h"
#include "config/configtree.h"
#include "enforce/import.h"
#include "pbo/pboreader.h"
#include "project.h"

#include <QAtomicInt>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QThread>

#include <utility>

namespace {

// ---------------------------------------------------------------- pbo access
//
// Everything in this file that knows what a pbo is goes through these two, so
// the reader stays the only module that has to understand the format.

struct PboHeaderInfo {
    bool ok = false;
    QString error;
    QString prefix;
    int entryCount = 0;
    QVector<PboEntry> scripts;   // the .c entries, which is all this wants
};

PboHeaderInfo readPboHeader(const QString &path)
{
    PboHeaderInfo info;
    PboFile pbo;
    if (!pbo.open(path, &info.error)) return info;
    info.prefix = pbo.prefix();
    info.entryCount = pbo.entries().size();
    for (const PboEntry &entry : pbo.entries())
        if (entry.name.endsWith(QLatin1String(".c"), Qt::CaseInsensitive))
            info.scripts.append(entry);
    info.ok = true;
    return info;
}

// What an entry writes out, which is its decompressed length rather than the
// length it occupies in the file. A size cap has to be measured against that or
// a packed script walks straight past it.
qint64 writtenSizeOf(const PboEntry &entry)
{
    return entry.originalSize ? qint64(entry.originalSize) : qint64(entry.dataSize);
}

// -------------------------------------------------------------- reading a mod

// mod.cpp and meta.cpp are flat key = value files, which the config parser
// already reads. It is not enough on its own: a real mod.cpp in the corpus has
// a line with no semicolon on it, and the parser then reads on to the next one
// and hands back two properties glued together. Every value in these files is
// one line, so a parsed value that spans lines is the sign of that, and the
// line scan below answers instead.
QString flatValue(const ConfigFile &parsed, const QString &text, const QString &key)
{
    for (const ConfigValue &value : parsed.values) {
        if (value.name.compare(key, Qt::CaseInsensitive) != 0 || value.scalar.isEmpty())
            continue;
        const QString scalar = configUnquote(value.scalar).trimmed();
        if (!scalar.contains('\n') && !scalar.contains('\r')) return scalar;
        break;
    }

    static const QString pattern = QStringLiteral("^[ \\t]*%1[ \\t]*=[ \\t]*(.*)$");
    QRegularExpression re(pattern.arg(QRegularExpression::escape(key)),
                          QRegularExpression::CaseInsensitiveOption
                              | QRegularExpression::MultilineOption);
    const QRegularExpressionMatch m = re.match(text);
    if (!m.hasMatch()) return {};

    QString raw = m.captured(1);
    // Trim a trailing comment and the semicolon, but only outside quotes: a
    // picture path is a quoted string full of backslashes and nothing else in
    // it may be treated as syntax.
    bool inQuote = false;
    int cut = raw.size();
    for (int i = 0; i < raw.size(); ++i) {
        const QChar c = raw.at(i);
        if (c == '"') inQuote = !inQuote;
        if (inQuote) continue;
        if (c == ';') { cut = i; break; }
        if (c == '/' && i + 1 < raw.size() && raw.at(i + 1) == '/') { cut = i; break; }
    }
    raw = raw.left(cut).trimmed();
    return configUnquote(raw).trimmed();
}

QString readTextFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(file.readAll());
}

// "Project3DPrinter_Scripts.pbo.TheProjectPublic.bisign" names TheProjectPublic.
QString signerOf(const QString &bisignName)
{
    if (!bisignName.endsWith(QLatin1String(".bisign"), Qt::CaseInsensitive)) return {};
    QString stem = bisignName.left(bisignName.size() - 7);
    const int pbo = stem.lastIndexOf(QLatin1String(".pbo."), -1, Qt::CaseInsensitive);
    if (pbo < 0) return {};
    return stem.mid(pbo + 5);
}

qint64 msecsOf(const QFileInfo &info)
{
    const QDateTime when = info.lastModified();
    return when.isValid() ? when.toMSecsSinceEpoch() : 0;
}

// ------------------------------------------------------------- the extraction

QString sanitise(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar c : name) {
        if (c.isLetterOrNumber() || c == '_' || c == '-' || c == '.') out.append(c);
        else if (out.isEmpty() || out.endsWith('_')) continue;
        else out.append('_');
    }
    while (out.endsWith('_')) out.chop(1);
    return out.isEmpty() ? QStringLiteral("mod") : out;
}

// A mod update has to land in a folder of its own, or a second open would read
// the previous version's scripts back out of the cache and show a graph of code
// that is no longer there.
QString cacheKeyFor(const ModEntry &mod)
{
    QCryptographicHash hash(QCryptographicHash::Sha1);
    hash.addData(mod.folder.toLower().toUtf8());
    hash.addData(QByteArray::number(mod.modified));
    const QString digest = QString::fromLatin1(hash.result().toHex().left(8));
    return sanitise(mod.folderName) + QLatin1Char('-') + digest;
}

// ------------------------------------------------------------ counting methods

void countMethods(const Graph &graph, int *asNodes, int *asText, int *empty)
{
    for (const GraphFunction &fn : graph.functions) {
        if (!fn.hasRawBody) { ++*asNodes; continue; }
        if (fn.rawBody.trimmed().isEmpty()) ++*empty;
        else ++*asText;
    }
    // A method that became an event or the constructor leaves no function
    // record behind, only its entry node, so those have to be counted from the
    // nodes. A declared function's entry node carries a "fn.entry." ref and is
    // already counted above.
    for (const GraphNode &node : graph.nodes) {
        if (node.kind == NodeKind::Event
            && !node.ref.startsWith(QLatin1String("fn.entry.")))
            ++*asNodes;
        else if (node.kind == NodeKind::Builtin && node.ref == bi::Begin)
            ++*asNodes;
    }
}

// A note list long enough to scroll past is a note list nobody reads, and the
// obfuscated archives in the corpus would supply a hundred thousand of them.
constexpr int kMaxNotes = 200;
// An archive whose entry table is mostly junk names is not worth walking to the
// end: nothing after the first few dozen refusals says anything new.
constexpr int kMaxRefusalsPerPbo = 32;

void addNote(ModOpenResult *result, const QString &text)
{
    if (result->notes.size() < kMaxNotes) result->notes.append(text);
    else result->notesDropped++;
}

int percentOf(int part, int whole)
{
    if (whole <= 0) return 0;
    return int(qRound(100.0 * double(part) / double(whole)));
}

const QString kReadOnlyKey = QStringLiteral("readOnly");
const QString kOriginKey = QStringLiteral("origin");

} // namespace

// ------------------------------------------------------------------ accessors

int ModEntry::scriptCount() const
{
    int n = 0;
    for (const ModPbo &pbo : pbos) n += pbo.scriptCount;
    return n;
}

int ModClassView::modelledPercent() const
{
    return percentOf(methodsAsNodes, methodsAsNodes + methodsAsText);
}

int ModOpenResult::modelledPercent() const
{
    return percentOf(methodsAsNodes, methodsAsNodes + methodsAsText);
}

// -------------------------------------------------------------- read only mark

void markGraphReadOnly(Graph &g, const QString &modName, const QString &pbo,
                       const QString &entry)
{
    g.extra.insert(kReadOnlyKey, true);
    QString origin = modName;
    if (!pbo.isEmpty()) origin += QStringLiteral(": ") + pbo;
    if (!entry.isEmpty()) {
        QString shown = entry;
        shown.replace('\\', '/');
        origin += QLatin1Char('/') + shown;
    }
    g.extra.insert(kOriginKey, origin);
}

bool graphIsReadOnly(const Graph &g)
{
    return g.extra.value(kReadOnlyKey).toBool(false);
}

QString graphOrigin(const Graph &g)
{
    return g.extra.value(kOriginKey).toString();
}

// ------------------------------------------------------------------- scanning

ModEntry ModLibrary::readMod(const QString &folder)
{
    ModEntry entry;
    const QFileInfo folderInfo(folder);
    if (!folderInfo.isDir()) return entry;

    entry.folder = QDir::cleanPath(folderInfo.absoluteFilePath());
    entry.folderName = folderInfo.fileName();
    const QString fallbackName =
        entry.folderName.startsWith('@') ? entry.folderName.mid(1) : entry.folderName;
    entry.name = fallbackName;
    entry.modified = msecsOf(folderInfo);

    const QStringList pboPaths = pbosUnder(entry.folder);

    // Whoever signed the pbos, used only when nothing says who wrote the mod.
    QString signer;
    for (const QString &pboPath : pboPaths) {
        const QFileInfo dir(pboPath);
        const QStringList signs = QDir(dir.absolutePath())
                                      .entryList({dir.fileName() + QStringLiteral(".*.bisign")},
                                                 QDir::Files);
        for (const QString &sign : signs) {
            const QString who = signerOf(sign);
            if (!who.isEmpty()) { signer = who; break; }
        }
        if (!signer.isEmpty()) break;
    }

    const QString modPath = entry.folder + QStringLiteral("/mod.cpp");
    if (QFileInfo::exists(modPath)) {
        entry.modified = qMax(entry.modified, msecsOf(QFileInfo(modPath)));
        const QString text = readTextFile(modPath);
        const ConfigFile parsed = parseConfig(text);
        const QString name = flatValue(parsed, text, QStringLiteral("name"));
        if (!name.isEmpty()) entry.name = name;
        entry.author = flatValue(parsed, text, QStringLiteral("author"));
        entry.version = flatValue(parsed, text, QStringLiteral("version"));
        entry.picture = flatValue(parsed, text, QStringLiteral("picture"));
        if (entry.picture.isEmpty())
            entry.picture = flatValue(parsed, text, QStringLiteral("logo"));
        entry.overview = flatValue(parsed, text, QStringLiteral("tooltip"));
        if (entry.overview.isEmpty())
            entry.overview = flatValue(parsed, text, QStringLiteral("overview"));
    }

    const QString metaPath = entry.folder + QStringLiteral("/meta.cpp");
    if (QFileInfo::exists(metaPath)) {
        entry.modified = qMax(entry.modified, msecsOf(QFileInfo(metaPath)));
        const QString text = readTextFile(metaPath);
        const ConfigFile parsed = parseConfig(text);
        const QString name = flatValue(parsed, text, QStringLiteral("name"));
        // mod.cpp wins: meta.cpp carries the workshop title, which is often the
        // listing name rather than what the mod calls itself in game.
        if (!name.isEmpty() && entry.name == fallbackName) entry.name = name;
        entry.publishedId = flatValue(parsed, text, QStringLiteral("publishedid"));
    }

    if (entry.author.isEmpty() && !signer.isEmpty()) {
        entry.author = signer;
        entry.authorIsSigner = true;
    }

    entry.overview = entry.overview.simplified();

    for (const QString &pboPath : pboPaths) {
        ModPbo pbo;
        pbo.path = pboPath;
        const QFileInfo fi(pboPath);
        pbo.size = fi.size();
        entry.modified = qMax(entry.modified, msecsOf(fi));

        const PboHeaderInfo info = readPboHeader(pboPath);
        pbo.readable = info.ok;
        pbo.error = info.error;
        pbo.prefix = info.prefix;
        pbo.entryCount = info.entryCount;
        pbo.scriptCount = info.scripts.size();
        if (!info.ok)
            entry.notes.append(QStringLiteral("%1: %2").arg(fi.fileName(), info.error));
        entry.pbos.append(pbo);
    }

    return entry;
}

QVector<ModEntry> ModLibrary::scanRoots(const QStringList &roots,
                                        const QHash<QString, ModEntry> &known,
                                        const std::function<bool()> &cancelled)
{
    QVector<ModEntry> out;
    QSet<QString> seen;

    for (const QString &rootPath : roots) {
        QDir root(rootPath);
        if (!root.exists()) continue;
        const QFileInfoList folders =
            root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &folder : folders) {
            if (cancelled && cancelled()) return out;
            // The workshop folder carries a marker directory beside the mods.
            if (folder.fileName().startsWith('!')) continue;
            const QString path = QDir::cleanPath(folder.absoluteFilePath());
            if (seen.contains(path.toLower())) continue;

            // Reusing a cached entry still costs the directory walk, because
            // the modified time is what decides whether it may be reused. What
            // it saves is the pbo header read, which is the expensive half.
            ModEntry fresh;
            const auto cached = known.constFind(path.toLower());
            if (cached != known.constEnd()) {
                qint64 stamp = msecsOf(folder);
                const QStringList files = pbosUnder(path);
                for (const QString &file : files) stamp = qMax(stamp, msecsOf(QFileInfo(file)));
                for (const QString &side : {QStringLiteral("/mod.cpp"),
                                            QStringLiteral("/meta.cpp")}) {
                    const QFileInfo fi(path + side);
                    if (fi.exists()) stamp = qMax(stamp, msecsOf(fi));
                }
                if (stamp == cached->modified && cached->pbos.size() == files.size())
                    fresh = *cached;
            }
            if (!fresh.isValid()) fresh = readMod(path);
            if (!fresh.isValid() || fresh.pbos.isEmpty()) continue;

            seen.insert(path.toLower());
            out.append(fresh);
        }
    }
    return out;
}

// ------------------------------------------------------------------- opening

ModOpenJob beginOpen(const ModEntry &mod, const ModOpenOptions &opts)
{
    ModOpenJob job;
    job.m_result.folder = mod.folder;
    job.m_result.modName = mod.name;

    if (!mod.isValid()) {
        job.m_result.error = QStringLiteral("no mod folder");
        job.m_done = true;
        return job;
    }

    const QString base = ModLibrary::scriptCacheRoot() + QLatin1Char('/') + cacheKeyFor(mod);
    qint64 bytes = 0;

    for (const ModPbo &info : mod.pbos) {
        if (!info.readable || info.scriptCount == 0) continue;
        const QString pboName = QFileInfo(info.path).fileName();

        // Opened once and held open, so pulling twenty scripts out of a 200 MB
        // archive is twenty seeks rather than twenty header walks.
        PboFile pbo;
        QString error;
        if (!pbo.open(info.path, &error)) {
            addNote(&job.m_result, QStringLiteral("%1: %2").arg(pboName, error));
            continue;
        }
        const QString pboBase = base + QLatin1Char('/')
                                + sanitise(QFileInfo(info.path).completeBaseName());

        int refusedHere = 0;
        bool flooded = false;
        for (const PboEntry &entry : pbo.entries()) {
            if (!entry.name.endsWith(QLatin1String(".c"), Qt::CaseInsensitive)) continue;
            if (opts.maxFiles > 0 && job.m_files.size() >= opts.maxFiles) {
                job.m_result.truncated = true;
                break;
            }
            if (opts.maxBytes > 0 && bytes >= opts.maxBytes) {
                job.m_result.truncated = true;
                break;
            }
            const qint64 size = writtenSizeOf(entry);
            if (opts.maxFileBytes > 0 && size > opts.maxFileBytes) {
                addNote(&job.m_result,
                        QStringLiteral("%1/%2: %3 KB of script, too big to lay out, skipped")
                            .arg(pboName, entry.name)
                            .arg(size / 1024));
                continue;
            }

            // The reader owns the rule for what an entry name may become,
            // because that is the check standing between a hostile archive and
            // the rest of the disk.
            QString reason;
            const QString rel = pboSafeRelativePath(entry.name, &reason);
            if (rel.isEmpty()) {
                addNote(&job.m_result,
                        QStringLiteral("%1/%2: %3, skipped").arg(pboName, entry.name, reason));
                job.m_result.filesRefused++;
                if (++refusedHere >= kMaxRefusalsPerPbo) { flooded = true; break; }
                continue;
            }
            const QString target = pboBase + QLatin1Char('/') + rel;

            // Already extracted at this version of the mod, so the bytes are the
            // same bytes: the cache key carries the modified time for this.
            const QFileInfo have(target);
            if (!have.exists() || have.size() != size) {
                QString readError;
                const QByteArray data = pbo.read(entry, &readError);
                if (!readError.isEmpty()) {
                    addNote(&job.m_result,
                            QStringLiteral("%1/%2: %3").arg(pboName, entry.name, readError));
                    job.m_result.filesRefused++;
                    if (++refusedHere >= kMaxRefusalsPerPbo) { flooded = true; break; }
                    continue;
                }
                QDir().mkpath(QFileInfo(target).absolutePath());
                QSaveFile out(target);
                if (!out.open(QIODevice::WriteOnly) || out.write(data) != data.size()
                    || !out.commit()) {
                    addNote(&job.m_result,
                            QStringLiteral("%1/%2: cannot write to the cache: %3")
                                .arg(pboName, entry.name, out.errorString()));
                    job.m_result.filesRefused++;
                    if (++refusedHere >= kMaxRefusalsPerPbo) { flooded = true; break; }
                    continue;
                }
            }

            ModScriptFile file;
            file.pbo = pboName;
            file.entry = entry.name;
            file.path = target;
            file.size = size;
            job.m_files.append(file);
            bytes += size;
        }
        if (flooded)
            addNote(&job.m_result,
                    QStringLiteral("%1: stopped after %2 entries this reader would not "
                                   "produce, the rest of its table reads the same way")
                        .arg(pboName)
                        .arg(kMaxRefusalsPerPbo));
        if (job.m_result.truncated) break;
    }

    job.m_result.files = job.m_files;
    job.m_result.filesExtracted = job.m_files.size();
    job.m_result.ok = !job.m_files.isEmpty();
    if (!job.m_result.ok) {
        job.m_done = true;
        job.m_result.error =
            job.m_result.notes.isEmpty()
                ? QStringLiteral("this mod ships no script files")
                : QStringLiteral("no script file in this mod could be read out of its pbo");
    }
    return job;
}

bool openModStep(ModOpenJob &job, const Catalog &cat, const Builtins &builtins,
                 const Project &project, int budget)
{
    if (job.m_done) return false;
    if (budget <= 0) budget = 1;

    for (int done = 0; done < budget && job.m_next < job.m_files.size(); ++done) {
        const ModScriptFile &file = job.m_files.at(job.m_next++);
        const ImportResult imported = importEnforceFile(file.path, cat, builtins, project);
        if (!imported.ok) {
            job.m_result.filesRefused++;
            addNote(&job.m_result,
                    QStringLiteral("%1/%2: %3").arg(file.pbo, file.entry, imported.error));
            continue;
        }
        if (imported.scripts.isEmpty()) {
            // A file of enums, defines or global functions. Not a failure, but
            // there is no class in it to open, and saying so beats an empty row.
            addNote(&job.m_result,
                    QStringLiteral("%1/%2: no class in this file, so there is no graph for it")
                        .arg(file.pbo, file.entry));
            continue;
        }

        job.m_result.filesImported++;
        for (const ImportedScript &script : imported.scripts) {
            ModClassView view;
            view.className = script.className;
            view.baseClass = script.baseClass;
            view.modded = script.modded;
            view.pbo = file.pbo;
            view.entry = file.entry;
            view.graph = script.graph;
            view.statementsLowered = script.statementsLowered;
            view.statementsTotal = script.statementsTotal;
            countMethods(view.graph, &view.methodsAsNodes, &view.methodsAsText,
                         &view.methodsEmpty);
            markGraphReadOnly(view.graph, job.m_result.modName, file.pbo, file.entry);

            job.m_result.statementsLowered += view.statementsLowered;
            job.m_result.statementsTotal += view.statementsTotal;
            job.m_result.methodsAsNodes += view.methodsAsNodes;
            job.m_result.methodsAsText += view.methodsAsText;
            job.m_result.methodsEmpty += view.methodsEmpty;
            job.m_result.classes.append(view);
        }
        for (const QString &note : imported.notes)
            addNote(&job.m_result,
                    QStringLiteral("%1/%2: %3").arg(file.pbo, file.entry, note));
    }

    if (job.m_next >= job.m_files.size()) job.m_done = true;
    return !job.m_done;
}

ModOpenResult openMod(const ModEntry &mod, const Catalog &cat, const Builtins &builtins,
                      const Project &project, const ModOpenOptions &opts)
{
    ModOpenJob job = beginOpen(mod, opts);
    while (openModStep(job, cat, builtins, project, 32)) { }
    return job.result();
}

// -------------------------------------------------------------- the scan thread

// Not in an anonymous namespace: moc has to see it to build a meta object, and
// a class it cannot name is a class it cannot generate one for.
class ScanThread : public QThread {
    Q_OBJECT
public:
    ScanThread(QStringList roots, QHash<QString, ModEntry> known, QObject *parent = nullptr)
        : QThread(parent), m_roots(std::move(roots)), m_known(std::move(known))
    {
    }

    void cancel() { m_cancel.storeRelaxed(1); }
    bool wasCancelled() const { return m_cancel.loadRelaxed() != 0; }
    QVector<ModEntry> take() { return std::move(m_found); }

signals:
    void progress(int done, int total);

protected:
    void run() override
    {
        int total = 0;
        for (const QString &rootPath : m_roots) {
            QDir root(rootPath);
            if (root.exists())
                total += root.entryList(QDir::Dirs | QDir::NoDotAndDotDot).size();
        }
        int done = 0;
        // Progress is reported per mod, but only every so often: 254 queued
        // signals in a burst cost the UI more than the scan does.
        QElapsedTimer since;
        since.start();
        m_found = ModLibrary::scanRoots(m_roots, m_known, [&] {
            if (++done % 8 == 0 || since.elapsed() > 200) {
                since.restart();
                emit progress(done, total);
            }
            return m_cancel.loadRelaxed() != 0;
        });
        emit progress(total, total);
    }

private:
    QStringList m_roots;
    QHash<QString, ModEntry> m_known;
    QVector<ModEntry> m_found;
    QAtomicInt m_cancel{0};
};

// --------------------------------------------------------------- the library

ModLibrary::ModLibrary(QObject *parent) : QObject(parent)
{
    m_roots = defaultRoots();
}

ModLibrary::~ModLibrary()
{
    if (m_scan) {
        static_cast<ScanThread *>(m_scan)->cancel();
        // The scan is cancelled between mods, and a mod costs a directory walk
        // plus a header read, so this returns rather than hangs.
        m_scan->wait(10000);
    }
}

QStringList ModLibrary::defaultRoots()
{
    // Steam puts subscribed mods in !Workshop beside the game, and the work
    // drive holds the ones built here. Neither path is a given, so this probes
    // rather than hardcodes one machine: the drive letter of a Steam library is
    // whichever disk had room at the time.
    QStringList candidates;
    const QByteArray fromEnv = qgetenv("DAYZ_WORKSHOP");
    if (!fromEnv.isEmpty()) candidates.append(QString::fromLocal8Bit(fromEnv));

    const QStringList layouts = {
        QStringLiteral("SteamLibrary/steamapps/common/DayZ/!Workshop"),
        QStringLiteral("Steam/steamapps/common/DayZ/!Workshop"),
        QStringLiteral("Program Files (x86)/Steam/steamapps/common/DayZ/!Workshop"),
        QStringLiteral("Games/SteamLibrary/steamapps/common/DayZ/!Workshop"),
    };
    for (const QFileInfo &drive : QDir::drives())
        for (const QString &layout : layouts)
            candidates.append(drive.absoluteFilePath() + layout);
    candidates.append(QStringLiteral("P:/Mods"));

    QStringList out;
    for (const QString &path : candidates) {
        const QString clean = QDir::cleanPath(path);
        if (!QFileInfo(clean).isDir()) continue;
        if (!out.contains(clean, Qt::CaseInsensitive)) out.append(clean);
    }
    return out;
}

void ModLibrary::setRoots(const QStringList &roots)
{
    QStringList clean;
    for (const QString &r : roots) {
        const QString path = QDir::cleanPath(r);
        if (!path.isEmpty() && !clean.contains(path, Qt::CaseInsensitive)) clean.append(path);
    }
    m_roots = clean;
}

bool ModLibrary::addRoot(const QString &folder)
{
    const QString path = QDir::cleanPath(folder);
    if (path.isEmpty() || !QFileInfo(path).isDir()) return false;
    if (m_roots.contains(path, Qt::CaseInsensitive)) return false;
    m_roots.append(path);
    return true;
}

bool ModLibrary::removeRoot(const QString &folder)
{
    const QString path = QDir::cleanPath(folder);
    for (int i = 0; i < m_roots.size(); ++i) {
        if (m_roots.at(i).compare(path, Qt::CaseInsensitive) == 0) {
            m_roots.removeAt(i);
            return true;
        }
    }
    return false;
}

const ModEntry *ModLibrary::mod(const QString &folder) const
{
    const QString path = QDir::cleanPath(folder);
    for (const ModEntry &entry : m_mods)
        if (entry.folder.compare(path, Qt::CaseInsensitive) == 0) return &entry;
    return nullptr;
}

bool ModLibrary::isScanning() const
{
    return m_scan != nullptr;
}

void ModLibrary::refresh(bool force)
{
    if (m_scan) return; // a scan is already running; let it finish

    QHash<QString, ModEntry> known;
    if (!force)
        for (const ModEntry &entry : m_mods) known.insert(entry.folder.toLower(), entry);

    auto *thread = new ScanThread(m_roots, known, this);
    m_scan = thread;
    connect(thread, &ScanThread::progress, this, &ModLibrary::scanProgress);
    connect(thread, &QThread::finished, this, &ModLibrary::onScanFinished);
    emit scanStarted();
    thread->start();
}

void ModLibrary::cancelScan()
{
    if (m_scan) static_cast<ScanThread *>(m_scan)->cancel();
}

void ModLibrary::onScanFinished()
{
    auto *thread = static_cast<ScanThread *>(m_scan);
    if (!thread) return;
    m_scan = nullptr;

    // finished() is emitted from inside run(), so the thread is only about to
    // stop. Joining here costs nothing and makes the delete below safe.
    thread->wait();
    const bool cancelled = thread->wasCancelled();
    const QVector<ModEntry> found = thread->take();
    thread->deleteLater();

    // A cancelled scan stopped partway, so what it found is a prefix of the
    // library rather than the library. Keeping what was already there beats
    // replacing 254 rows with the first thirty.
    if (!cancelled) {
        m_mods = found;
        saveCache();
        emit modsChanged();
    }
    emit scanFinished(m_mods.size(), cancelled);
}

// ------------------------------------------------------------------ the cache

QString ModLibrary::cacheFile() const
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : dir + QStringLiteral("/modlibrary.json");
}

QString ModLibrary::scriptCacheRoot()
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return dir.isEmpty() ? QString() : QDir::cleanPath(dir + QStringLiteral("/modscripts"));
}

bool ModLibrary::isInsideScriptCache(const QString &path)
{
    const QString root = scriptCacheRoot();
    if (root.isEmpty()) return false;
    const QString full = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    return full.startsWith(root + QLatin1Char('/'), Qt::CaseInsensitive);
}

void ModLibrary::clearScriptCache()
{
    const QString root = scriptCacheRoot();
    if (root.isEmpty()) return;
    QDir(root).removeRecursively();
}

namespace {

QJsonObject pboToJson(const ModPbo &pbo)
{
    QJsonObject o;
    o.insert(QStringLiteral("path"), pbo.path);
    o.insert(QStringLiteral("prefix"), pbo.prefix);
    o.insert(QStringLiteral("size"), double(pbo.size));
    o.insert(QStringLiteral("entries"), pbo.entryCount);
    o.insert(QStringLiteral("scripts"), pbo.scriptCount);
    o.insert(QStringLiteral("readable"), pbo.readable);
    if (!pbo.error.isEmpty()) o.insert(QStringLiteral("error"), pbo.error);
    return o;
}

ModPbo pboFromJson(const QJsonObject &o)
{
    ModPbo pbo;
    pbo.path = o.value(QStringLiteral("path")).toString();
    pbo.prefix = o.value(QStringLiteral("prefix")).toString();
    pbo.size = qint64(o.value(QStringLiteral("size")).toDouble());
    pbo.entryCount = o.value(QStringLiteral("entries")).toInt();
    pbo.scriptCount = o.value(QStringLiteral("scripts")).toInt();
    pbo.readable = o.value(QStringLiteral("readable")).toBool();
    pbo.error = o.value(QStringLiteral("error")).toString();
    return pbo;
}

} // namespace

bool ModLibrary::saveCache(QString *error) const
{
    const QString path = cacheFile();
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("no writable application data folder");
        return false;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonArray mods;
    for (const ModEntry &entry : m_mods) {
        QJsonObject o;
        o.insert(QStringLiteral("folder"), entry.folder);
        o.insert(QStringLiteral("folderName"), entry.folderName);
        o.insert(QStringLiteral("name"), entry.name);
        if (!entry.author.isEmpty()) o.insert(QStringLiteral("author"), entry.author);
        if (entry.authorIsSigner) o.insert(QStringLiteral("authorIsSigner"), true);
        if (!entry.version.isEmpty()) o.insert(QStringLiteral("version"), entry.version);
        if (!entry.picture.isEmpty()) o.insert(QStringLiteral("picture"), entry.picture);
        if (!entry.overview.isEmpty()) o.insert(QStringLiteral("overview"), entry.overview);
        if (!entry.publishedId.isEmpty())
            o.insert(QStringLiteral("publishedId"), entry.publishedId);
        o.insert(QStringLiteral("modified"), double(entry.modified));
        QJsonArray pbos;
        for (const ModPbo &pbo : entry.pbos) pbos.append(pboToJson(pbo));
        o.insert(QStringLiteral("pbos"), pbos);
        if (!entry.notes.isEmpty())
            o.insert(QStringLiteral("notes"), QJsonArray::fromStringList(entry.notes));
        mods.append(o);
    }

    QJsonObject root;
    root.insert(QStringLiteral("version"), 1);
    root.insert(QStringLiteral("roots"), QJsonArray::fromStringList(m_roots));
    root.insert(QStringLiteral("mods"), mods);

    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

bool ModLibrary::loadCache(QString *error)
{
    const QString path = cacheFile();
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("no writable application data folder");
        return false;
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    QJsonParseError parse{};
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
        if (error) *error = parse.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("version")).toInt() != 1) {
        if (error) *error = QStringLiteral("cache was written by another version");
        return false;
    }

    const QJsonArray roots = root.value(QStringLiteral("roots")).toArray();
    if (!roots.isEmpty()) {
        QStringList list;
        for (const QJsonValue &v : roots) list.append(v.toString());
        setRoots(list);
    }

    QVector<ModEntry> mods;
    const QJsonArray array = root.value(QStringLiteral("mods")).toArray();
    for (const QJsonValue &v : array) {
        const QJsonObject o = v.toObject();
        ModEntry entry;
        entry.folder = o.value(QStringLiteral("folder")).toString();
        if (entry.folder.isEmpty()) continue;
        entry.folderName = o.value(QStringLiteral("folderName")).toString();
        entry.name = o.value(QStringLiteral("name")).toString();
        entry.author = o.value(QStringLiteral("author")).toString();
        entry.authorIsSigner = o.value(QStringLiteral("authorIsSigner")).toBool();
        entry.version = o.value(QStringLiteral("version")).toString();
        entry.picture = o.value(QStringLiteral("picture")).toString();
        entry.overview = o.value(QStringLiteral("overview")).toString();
        entry.publishedId = o.value(QStringLiteral("publishedId")).toString();
        entry.modified = qint64(o.value(QStringLiteral("modified")).toDouble());
        for (const QJsonValue &p : o.value(QStringLiteral("pbos")).toArray())
            entry.pbos.append(pboFromJson(p.toObject()));
        for (const QJsonValue &n : o.value(QStringLiteral("notes")).toArray())
            entry.notes.append(n.toString());
        mods.append(entry);
    }

    m_mods = mods;
    emit modsChanged();
    return true;
}

#include "modlibrary.moc"
