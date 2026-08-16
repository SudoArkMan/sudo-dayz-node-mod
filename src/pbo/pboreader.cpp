#include "pbo/pboreader.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QProcess>
#include <QSet>

namespace {

// Longer than any path Windows will take, so a real name never trips it and a
// header that has lost sync stops inside one buffer instead of eating the file.
constexpr int kMaxNameBytes = 4096;
// A product header carries free text. The obfuscator on this machine writes a
// banner of several hundred bytes as a key, so the ceiling is generous.
constexpr int kMaxHeaderTextBytes = 1 << 16;
// One entry materialised in memory. Extraction streams anything larger when it
// is stored rather than packed, so this only bounds what read() will hand back.
constexpr qint64 kMaxEntryBytes = 512LL << 20;
// A junk archive would otherwise report a refusal per entry, half a million
// times.
constexpr int kMaxRefusalsReported = 32;
constexpr qint64 kCopyChunk = 1 << 20;

// LZSS spends at most 17 input bytes, eight pair tokens plus the flag byte that
// covers them, to produce 144 output bytes. Anything past that ratio is a lie
// about originalSize, and the only thing believing it would buy is a large
// allocation on behalf of a hostile file.
qint64 maxExpansion(qint64 packedBytes)
{
    return packedBytes * 9 + 64;
}

// A forward cursor over the header. The header of an obfuscated pbo on this
// machine is 150 MB, so it is read in chunks instead of being slurped.
class HeaderCursor {
public:
    explicit HeaderCursor(QFile &file) : m_file(file) {}

    bool readString(QByteArray *out, int cap, QString *error)
    {
        out->clear();
        for (;;) {
            if (m_pos >= m_buf.size() && !refill()) {
                *error = QStringLiteral("header runs past the end of the file");
                return false;
            }
            const int start = m_pos;
            while (m_pos < m_buf.size() && m_buf.at(m_pos) != '\0') ++m_pos;
            out->append(m_buf.constData() + start, m_pos - start);
            if (out->size() > cap) {
                *error = QStringLiteral("header string longer than %1 bytes").arg(cap);
                return false;
            }
            if (m_pos < m_buf.size()) { ++m_pos; return true; }
        }
    }

    bool readU32(quint32 *out, QString *error)
    {
        quint32 value = 0;
        for (int i = 0; i < 4; ++i) {
            if (m_pos >= m_buf.size() && !refill()) {
                *error = QStringLiteral("header runs past the end of the file");
                return false;
            }
            value |= quint32(uchar(m_buf.at(m_pos++))) << (8 * i);
        }
        *out = value;
        return true;
    }

    qint64 pos() const { return m_base + m_pos; }

private:
    bool refill()
    {
        m_base += m_buf.size();
        m_buf.resize(64 * 1024);
        const qint64 n = m_file.read(m_buf.data(), m_buf.size());
        if (n <= 0) { m_buf.clear(); m_pos = 0; return false; }
        m_buf.resize(int(n));
        m_pos = 0;
        return true;
    }

    QFile &m_file;
    QByteArray m_buf;
    int m_pos = 0;
    qint64 m_base = 0;
};

QString normaliseName(const QString &name)
{
    QString s = name;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));
    return s.toLower();
}

// Names Windows resolves to a device no matter what folder they sit in, and no
// matter what extension is stuck on the end.
bool isReservedDeviceName(const QString &segment)
{
    static const QSet<QString> reserved = {
        QStringLiteral("con"), QStringLiteral("prn"), QStringLiteral("aux"),
        QStringLiteral("nul"), QStringLiteral("com1"), QStringLiteral("com2"),
        QStringLiteral("com3"), QStringLiteral("com4"), QStringLiteral("com5"),
        QStringLiteral("com6"), QStringLiteral("com7"), QStringLiteral("com8"),
        QStringLiteral("com9"), QStringLiteral("lpt1"), QStringLiteral("lpt2"),
        QStringLiteral("lpt3"), QStringLiteral("lpt4"), QStringLiteral("lpt5"),
        QStringLiteral("lpt6"), QStringLiteral("lpt7"), QStringLiteral("lpt8"),
        QStringLiteral("lpt9"),
    };
    const int dot = segment.indexOf(QLatin1Char('.'));
    const QString stem = (dot < 0 ? segment : segment.left(dot)).toLower();
    return reserved.contains(stem);
}

void noteRefusal(PboExtractReport *report, const QString &name, const QString &reason)
{
    if (!report) return;
    report->refused++;
    if (report->refusals.size() < kMaxRefusalsReported)
        report->refusals << (name + QStringLiteral(": ") + reason);
}

}  // namespace

const QStringList &pboReadableSuffixes()
{
    static const QStringList suffixes = {
        QStringLiteral(".c"), QStringLiteral(".cpp"), QStringLiteral(".xml"),
        QStringLiteral(".json"), QStringLiteral(".csv"), QStringLiteral(".layout"),
    };
    return suffixes;
}

QByteArray pboDecompress(const QByteArray &packed, quint32 originalSize, QString *error)
{
    if (error) error->clear();
    const auto fail = [&](const QString &why) {
        if (error) *error = why;
        return QByteArray();
    };
    if (originalSize == 0) return QByteArray();
    if (qint64(originalSize) > maxExpansion(packed.size()))
        return fail(QStringLiteral("claims %1 bytes out of %2 packed, past what LZSS can expand")
                        .arg(originalSize).arg(packed.size()));
    if (qint64(originalSize) > kMaxEntryBytes)
        return fail(QStringLiteral("entry of %1 bytes is larger than this reads at once")
                        .arg(originalSize));

    QByteArray out;
    out.resize(qsizetype(originalSize));
    uchar *dst = reinterpret_cast<uchar *>(out.data());
    const uchar *in = reinterpret_cast<const uchar *>(packed.constData());
    const qint64 inSize = packed.size();

    qint64 si = 0;
    quint32 produced = 0;
    quint32 flags = 0;
    quint32 sum = 0;
    while (produced < originalSize) {
        // Eight tokens share one flag byte, low bit first. The 0xFF00 marker
        // rides along and shifts out after the eighth, which is what says the
        // next byte is another flag rather than data.
        flags >>= 1;
        if ((flags & 0x100u) == 0) {
            if (si >= inSize)
                return fail(QStringLiteral("packed data ends mid stream, %1 of %2 bytes decoded")
                                .arg(produced).arg(originalSize));
            flags = quint32(in[si++]) | 0xFF00u;
        }
        if (flags & 1u) {
            if (si >= inSize)
                return fail(QStringLiteral("packed data ends mid stream, %1 of %2 bytes decoded")
                                .arg(produced).arg(originalSize));
            const uchar c = in[si++];
            sum += c;
            dst[produced++] = c;
            continue;
        }
        if (si + 1 >= inSize)
            return fail(QStringLiteral("packed data ends mid stream, %1 of %2 bytes decoded")
                            .arg(produced).arg(originalSize));
        const quint32 b1 = in[si++];
        const quint32 b2 = in[si++];
        // The pair is a distance back into what has already been produced, not
        // an index into a window. That is the one place this differs from
        // textbook LZSS, and getting it wrong decodes the first few hundred
        // bytes of a file correctly before turning to noise.
        const qint64 distance = qint64(b1 | ((b2 & 0xF0u) << 4));
        const int length = int(b2 & 0x0Fu) + 3;
        const qint64 from = qint64(produced) - distance;
        for (int k = 0; k < length && produced < originalSize; ++k) {
            const qint64 at = from + k;
            // Reading behind the start of the output, or the byte a distance of
            // zero points at, means the reference encoder's window: it starts
            // full of spaces, and 3985 entries in the installed mods rely on it.
            const uchar c = (at < 0 || at >= qint64(produced)) ? uchar(0x20) : dst[at];
            sum += c;
            dst[produced++] = c;
        }
    }

    // Four bytes of checksum follow the stream, the sum of the decoded bytes.
    // It is the only proof the decoder got it right, so a mismatch refuses the
    // entry. Twenty entries in the installed mods stop without one, and those
    // are taken as they are.
    if (si + 4 <= inSize) {
        const quint32 stored = quint32(in[si]) | (quint32(in[si + 1]) << 8)
            | (quint32(in[si + 2]) << 16) | (quint32(in[si + 3]) << 24);
        if (stored != sum)
            return fail(QStringLiteral("checksum is %1, decoded bytes sum to %2")
                            .arg(stored).arg(sum));
    }
    return out;
}

QString pboSafeRelativePath(const QString &entryName, QString *reason)
{
    const auto refuse = [&](const QString &why) {
        if (reason) *reason = why;
        return QString();
    };
    if (reason) reason->clear();
    if (entryName.isEmpty()) return refuse(QStringLiteral("empty name"));
    if (entryName.size() > kMaxNameBytes) return refuse(QStringLiteral("name is absurdly long"));

    // Both separators are treated as separators. A name that mixes them cannot
    // then smuggle a segment past a check that only looked at one of them.
    QString s = entryName;
    s.replace(QLatin1Char('\\'), QLatin1Char('/'));

    if (s.startsWith(QLatin1Char('/'))) return refuse(QStringLiteral("absolute path"));
    if (s.size() >= 2 && s.at(1) == QLatin1Char(':'))
        return refuse(QStringLiteral("drive letter"));

    const QStringList parts = s.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QStringList kept;
    for (const QString &part : parts) {
        if (part == QLatin1String(".")) continue;
        if (part == QLatin1String(".."))
            return refuse(QStringLiteral("parent directory segment"));
        if (part.endsWith(QLatin1Char('.')) || part.endsWith(QLatin1Char(' ')))
            return refuse(QStringLiteral("segment Windows would rename"));
        for (const QChar ch : part) {
            const ushort u = ch.unicode();
            if (u < 0x20 || u == 0x7f)
                return refuse(QStringLiteral("control character in the name"));
            if (ch == QLatin1Char('<') || ch == QLatin1Char('>') || ch == QLatin1Char(':')
                || ch == QLatin1Char('"') || ch == QLatin1Char('|') || ch == QLatin1Char('?')
                || ch == QLatin1Char('*'))
                return refuse(QStringLiteral("character Windows reserves"));
        }
        if (isReservedDeviceName(part))
            return refuse(QStringLiteral("name Windows reserves for a device"));
        kept << part;
    }
    if (kept.isEmpty()) return refuse(QStringLiteral("name has no usable segment"));
    return kept.join(QLatin1Char('/'));
}

PboFile::PboFile() = default;

PboFile::~PboFile()
{
    close();
}

bool PboFile::isOpen() const
{
    return m_file.isOpen();
}

void PboFile::close()
{
    if (m_file.isOpen()) m_file.close();
    m_path.clear();
    m_size = 0;
    m_prefix.clear();
    m_headers.clear();
    m_entries.clear();
    m_byName.clear();
    m_reconciles = false;
}

bool PboFile::open(const QString &path, QString *error)
{
    close();
    if (error) error->clear();
    const auto fail = [&](const QString &why) {
        if (error) *error = why;
        close();
        return false;
    };

    m_file.setFileName(path);
    if (!m_file.open(QIODevice::ReadOnly)) return fail(m_file.errorString());
    m_path = path;
    m_size = m_file.size();
    // The smallest archive still carries a terminating entry of 21 bytes, one
    // zero byte and a 20 byte SHA1.
    if (m_size < 42) return fail(QStringLiteral("file is too small to be a pbo"));

    HeaderCursor cursor(m_file);
    QString why;
    bool first = true;
    // Every entry costs at least 21 bytes of header, so the length of the file
    // is the only ceiling needed. That accepts the obfuscated mods, which do
    // carry a million entries each, and refuses a header claiming more than the
    // file could hold.
    const qint64 entryCeiling = m_size / 21 + 1;

    for (;;) {
        QByteArray rawName;
        if (!cursor.readString(&rawName, kMaxNameBytes, &why)) return fail(why);
        quint32 mime = 0, original = 0, reserved = 0, stamp = 0, data = 0;
        if (!cursor.readU32(&mime, &why) || !cursor.readU32(&original, &why)
            || !cursor.readU32(&reserved, &why) || !cursor.readU32(&stamp, &why)
            || !cursor.readU32(&data, &why))
            return fail(why);
        Q_UNUSED(reserved);

        if (first && rawName.isEmpty() && mime == kPboMimeProduct) {
            first = false;
            for (;;) {
                QByteArray key;
                if (!cursor.readString(&key, kMaxHeaderTextBytes, &why)) return fail(why);
                if (key.isEmpty()) break;
                QByteArray value;
                if (!cursor.readString(&value, kMaxHeaderTextBytes, &why)) return fail(why);
                // Latin-1 rather than UTF-8: it maps every byte to exactly one
                // character, so a junk header cannot be mangled into something
                // that no longer matches what is on disk.
                const QString k = QString::fromLatin1(key);
                m_headers.insert(k, QString::fromLatin1(value));
                if (k.compare(QLatin1String("prefix"), Qt::CaseInsensitive) == 0)
                    m_prefix = QString::fromLatin1(value);
            }
            continue;
        }
        first = false;
        if (rawName.isEmpty()) break;  // the terminating entry

        PboEntry entry;
        entry.name = QString::fromLatin1(rawName);
        entry.mime = mime;
        entry.originalSize = original;
        entry.dataSize = data;
        entry.timestamp = stamp;
        m_entries.append(entry);
        if (m_entries.size() > entryCeiling)
            return fail(QStringLiteral("header lists more entries than the file could hold"));
    }

    qint64 at = cursor.pos();
    for (PboEntry &entry : m_entries) {
        entry.offset = at;
        at += qint64(entry.dataSize);   // qint64 so a 2 GB archive cannot wrap
        if (at > m_size)
            return fail(QStringLiteral("entry '%1' runs %2 bytes past the end of the file")
                            .arg(entry.name).arg(at - m_size));
    }
    m_reconciles = (m_size - at) == 21;

    // A packed archive repeats the same name thousands of times: one mod here
    // carries 6069 entries whose names collide. The first one wins the lookup,
    // matching how the rest of this project resolves a path, and entries() still
    // holds every one of them for a caller that wants to see the collision.
    m_byName.reserve(m_entries.size());
    for (int i = 0; i < m_entries.size(); ++i) {
        const QString key = normaliseName(m_entries.at(i).name);
        if (!m_byName.contains(key)) m_byName.insert(key, i);
    }
    return true;
}

const PboEntry *PboFile::find(const QString &name) const
{
    const auto it = m_byName.constFind(normaliseName(name));
    if (it == m_byName.constEnd()) return nullptr;
    return &m_entries.at(it.value());
}

QByteArray PboFile::read(const PboEntry &entry, QString *error) const
{
    if (error) error->clear();
    const auto fail = [&](const QString &why) {
        if (error) *error = why;
        return QByteArray();
    };
    if (!m_file.isOpen()) return fail(QStringLiteral("archive is not open"));
    if (entry.offset < 0 || entry.offset + qint64(entry.dataSize) > m_size)
        return fail(QStringLiteral("entry lies outside the file"));
    if (entry.dataSize == 0) return QByteArray();
    if (qint64(entry.dataSize) > kMaxEntryBytes)
        return fail(QStringLiteral("entry of %1 bytes is larger than this reads at once")
                        .arg(entry.dataSize));

    if (!m_file.seek(entry.offset)) return fail(m_file.errorString());
    const QByteArray raw = m_file.read(qint64(entry.dataSize));
    if (raw.size() != qsizetype(entry.dataSize))
        return fail(QStringLiteral("read %1 of %2 bytes").arg(raw.size()).arg(entry.dataSize));
    if (!entry.compressed()) return raw;

    QString whyDecode;
    const QByteArray out = pboDecompress(raw, entry.originalSize, &whyDecode);
    if (!whyDecode.isEmpty()) return fail(whyDecode);
    return out;
}

QByteArray PboFile::read(const QString &name, QString *error) const
{
    const PboEntry *entry = find(name);
    if (!entry) {
        if (error) *error = QStringLiteral("no entry named '%1'").arg(name);
        return QByteArray();
    }
    return read(*entry, error);
}

QStringList PboFile::filesMatching(const QString &suffix) const
{
    return filesMatching(QStringList{suffix});
}

QStringList PboFile::filesMatching(const QStringList &suffixes) const
{
    QStringList out;
    for (const PboEntry &entry : m_entries) {
        for (const QString &suffix : suffixes) {
            if (entry.name.endsWith(suffix, Qt::CaseInsensitive)) { out << entry.name; break; }
        }
    }
    return out;
}

bool PboFile::extractOne(const PboEntry &entry, const QString &destDir,
                         PboExtractReport *report) const
{
    QString reason;
    const QString relative = pboSafeRelativePath(entry.name, &reason);
    if (relative.isEmpty()) {
        noteRefusal(report, entry.name, reason);
        return false;
    }
    const QString target = destDir + QLatin1Char('/') + relative;
    // The sanitiser already refused every way out of the folder. This is the
    // second look, taken against the path that is about to be opened, because
    // path traversal on extract is the failure that damages a machine.
    const QString cleanTarget = QDir::cleanPath(target);
    const QString cleanDest = QDir::cleanPath(destDir);
    if (!cleanTarget.startsWith(cleanDest + QLatin1Char('/'))) {
        noteRefusal(report, entry.name, QStringLiteral("resolves outside the destination"));
        return false;
    }

    const QFileInfo info(cleanTarget);
    if (!QDir().mkpath(info.absolutePath())) {
        noteRefusal(report, entry.name, QStringLiteral("could not create the folder"));
        return false;
    }
    QFile out(cleanTarget);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        noteRefusal(report, entry.name, out.errorString());
        return false;
    }

    if (!entry.compressed()) {
        // Stored entries are streamed so a large model does not have to fit in
        // memory to be written to disk.
        if (!m_file.seek(entry.offset)) {
            noteRefusal(report, entry.name, m_file.errorString());
            return false;
        }
        qint64 left = qint64(entry.dataSize);
        QByteArray buffer;
        while (left > 0) {
            const qint64 want = qMin(left, kCopyChunk);
            buffer = m_file.read(want);
            if (buffer.size() != want) {
                noteRefusal(report, entry.name, QStringLiteral("short read"));
                out.close();
                QFile::remove(cleanTarget);
                return false;
            }
            if (out.write(buffer) != buffer.size()) {
                noteRefusal(report, entry.name, out.errorString());
                out.close();
                QFile::remove(cleanTarget);
                return false;
            }
            left -= want;
        }
    } else {
        QString why;
        const QByteArray bytes = read(entry, &why);
        if (!why.isEmpty()) {
            noteRefusal(report, entry.name, why);
            out.close();
            QFile::remove(cleanTarget);
            return false;
        }
        if (out.write(bytes) != bytes.size()) {
            noteRefusal(report, entry.name, out.errorString());
            out.close();
            QFile::remove(cleanTarget);
            return false;
        }
    }
    out.close();
    if (report) report->written++;
    return true;
}

bool PboFile::extract(const QString &destDir, const QStringList &suffixes,
                      PboExtractReport *report, QString *error) const
{
    if (error) error->clear();
    if (!m_file.isOpen()) {
        if (error) *error = QStringLiteral("archive is not open");
        return false;
    }
    if (!QDir().mkpath(destDir)) {
        if (error) *error = QStringLiteral("could not create '%1'").arg(destDir);
        return false;
    }
    for (const PboEntry &entry : m_entries) {
        if (!suffixes.isEmpty()) {
            bool wanted = false;
            for (const QString &suffix : suffixes) {
                if (entry.name.endsWith(suffix, Qt::CaseInsensitive)) { wanted = true; break; }
            }
            if (!wanted) { if (report) report->skipped++; continue; }
        }
        extractOne(entry, destDir, report);
    }
    return true;
}

bool PboFile::extractAll(const QString &destDir, PboExtractReport *report, QString *error) const
{
    return extract(destDir, QStringList(), report, error);
}

bool PboFile::extractScripts(const QString &destDir, PboExtractReport *report,
                             QString *error) const
{
    return extract(destDir, pboReadableSuffixes(), report, error);
}

QStringList pbosUnder(const QString &root, int maxDepth)
{
    QStringList found;
    QSet<QString> visited;
    QStringList queue;
    QVector<int> depths;
    queue << root;
    depths << 0;
    while (!queue.isEmpty()) {
        const QString dir = queue.takeFirst();
        const int depth = depths.takeFirst();
        // Mods under !Workshop are directory junctions, so the walk has to
        // resolve links. Remembering where it has been is what stops a link
        // pointing at its own parent from spinning.
        const QString key = QFileInfo(dir).canonicalFilePath();
        if (key.isEmpty() || visited.contains(key)) continue;
        visited.insert(key);

        const QFileInfoList items = QDir(dir).entryInfoList(
            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
        for (const QFileInfo &item : items) {
            if (item.isDir()) {
                if (depth < maxDepth) { queue << item.absoluteFilePath(); depths << depth + 1; }
            } else if (item.fileName().endsWith(QLatin1String(".pbo"), Qt::CaseInsensitive)) {
                found << item.absoluteFilePath();
            }
        }
    }
    found.sort(Qt::CaseInsensitive);
    return found;
}

QString pboBankRevPath()
{
    QStringList candidates;
    const QByteArray fromEnv = qgetenv("DAYZ_TOOLS_BIN");
    if (!fromEnv.isEmpty())
        candidates << QString::fromLocal8Bit(fromEnv) + QStringLiteral("/PboUtils/BankRev.exe");
    candidates << QStringLiteral("D:/SteamLibrary/steamapps/common/DayZ Tools/Bin/PboUtils/BankRev.exe")
               << QStringLiteral("C:/Program Files (x86)/Steam/steamapps/common/DayZ Tools/Bin/PboUtils/BankRev.exe");
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) return candidate;
    }
    return QString();
}

bool pboExtractWithBankRev(const QString &pboPath, const QString &destDir, QString *error,
                           int timeoutMs)
{
    if (error) error->clear();
    const QString exe = pboBankRevPath();
    if (exe.isEmpty()) {
        if (error) *error = QStringLiteral("BankRev.exe is not on this machine");
        return false;
    }
    if (!QDir().mkpath(destDir)) {
        if (error) *error = QStringLiteral("could not create '%1'").arg(destDir);
        return false;
    }
    // BankRev takes native separators. Given a forward slash path it creates a
    // folder whose name contains the slashes rather than extracting anything.
    QProcess process;
    process.start(exe, QStringList{QStringLiteral("-f"), QDir::toNativeSeparators(destDir),
                                   QDir::toNativeSeparators(pboPath)});
    if (!process.waitForStarted(10000)) {
        if (error) *error = QStringLiteral("BankRev.exe would not start");
        return false;
    }
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        process.waitForFinished(2000);
        if (error) *error = QStringLiteral("BankRev.exe did not finish in %1 ms").arg(timeoutMs);
        return false;
    }
    const QString said = (QString::fromLocal8Bit(process.readAllStandardOutput())
                          + QString::fromLocal8Bit(process.readAllStandardError())).trimmed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("BankRev.exe exited %1: %2").arg(process.exitCode()).arg(said);
        return false;
    }
    // BankRev exits zero on an archive it refuses. It says "unable to open ...
    // Obfuscated PBO." and writes nothing, so the folder is what decides
    // whether it worked, not the exit code.
    QDirIterator written(destDir, QDir::Files, QDirIterator::Subdirectories);
    if (!written.hasNext()) {
        if (error)
            *error = said.isEmpty() ? QStringLiteral("BankRev.exe wrote nothing")
                                    : said.section(QLatin1Char('\n'), -1).trimmed();
        return false;
    }
    return true;
}
