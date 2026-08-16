// PBO archives, read in this process.
//
// A DayZ mod ships its scripts inside .pbo archives, so "open an installed mod
// and show me its graphs" starts here. Shelling out to BankRev.exe works on a
// machine with DayZ Tools and nowhere else, and it cannot run inside a headless
// test, so the format is read directly and BankRev is kept for the entries this
// refuses.
//
// Layout, confirmed by reading the bytes of an installed mod:
//
//   product entry  empty name, mime "Vers". The four bytes on disk spell "sreV",
//                  which is 0x56657273 read little endian. Then NUL terminated
//                  key/value pairs ending in an empty key. `prefix` is the path
//                  the engine mounts the archive under, and it is what makes a
//                  script inside resolve to Project3DPrinter_Scripts/... .
//   entries        NUL terminated name, then mime, original size, reserved,
//                  timestamp and data size as little endian uint32.
//   terminator     an entry whose name is empty.
//   data           every entry's bytes back to back, in entry order.
//   trailer        one zero byte and a 20 byte SHA1 of everything before it.
//
// Every field here came off the internet. A mod is a file somebody uploaded,
// and 104 of the 1874 archives installed on this machine went through a packer
// that writes junk entries carrying control characters and `..` in their names,
// one of them a million of them. Sizes are checked against the length of the
// file, names are checked before anything is written, and an entry whose bytes
// cannot be reproduced exactly is refused with a reason rather than guessed at.
//
// Measured over those 1874 archives: all 1874 open, 9,343,981 packed entries
// decode and match the checksum each one carries, and the 648 entries sampled
// against BankRev.exe come back byte for byte. BankRev turns down every one of
// the 104 packed archives; this reads them.
#pragma once

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QString>
#include <QStringList>
#include <QVector>

// The two packing methods that appear. Both are the ASCII tag read as a little
// endian uint32, which is why the bytes on disk look reversed.
constexpr quint32 kPboMimeProduct = 0x56657273;   // "Vers", the header entry
constexpr quint32 kPboMimeCompressed = 0x43707273;  // "Cprs", LZSS packed

struct PboEntry {
    QString name;        // path inside the pbo, backslashes as written
    quint32 mime = 0;
    quint32 originalSize = 0;
    quint32 dataSize = 0;
    quint32 timestamp = 0;
    qint64 offset = 0;   // where its bytes start in the file
    bool compressed() const { return mime == kPboMimeCompressed; }
};

// What an extract did, so a caller can say which entries it would not write and
// why instead of reporting a bare false.
struct PboExtractReport {
    int written = 0;
    int skipped = 0;   // filtered out by the suffix list, not a problem
    int refused = 0;   // the name or the bytes were not safe to produce
    QStringList refusals;  // "name: reason", capped so a junk pbo cannot flood
};

// One archive. Holds the file open between reads so pulling one script out of a
// 200 MB pbo is a seek and a read rather than a load. Not reentrant: one
// instance belongs to one thread.
class PboFile {
public:
    PboFile();
    ~PboFile();
    PboFile(const PboFile &) = delete;
    PboFile &operator=(const PboFile &) = delete;

    bool open(const QString &path, QString *error = nullptr);
    void close();
    bool isOpen() const;

    QString path() const { return m_path; }
    qint64 fileSize() const { return m_size; }
    QString prefix() const { return m_prefix; }          // from the Vers header
    QHash<QString, QString> headers() const { return m_headers; }  // every key/value
    const QVector<PboEntry> &entries() const { return m_entries; }

    // The header, the data blobs and the 21 byte trailer add up to the length of
    // the file. False means the archive carries something this does not model,
    // which is worth reporting but is not a reason to refuse the entries: the
    // offsets were all checked against the file either way.
    bool reconciles() const { return m_reconciles; }

    // Slashes and case are both normalised, because a name is written with
    // backslashes inside the pbo and typed with forward slashes everywhere else.
    // A packed archive can carry the same name many times over, and the first
    // one wins here; entries() still lists every copy.
    const PboEntry *find(const QString &name) const;

    // Decompressed. Empty with an error set means refused; empty with no error
    // means the entry really is empty.
    QByteArray read(const PboEntry &entry, QString *error = nullptr) const;
    QByteArray read(const QString &name, QString *error = nullptr) const;

    QStringList filesMatching(const QString &suffix) const;   // ".c", ".cpp"
    QStringList filesMatching(const QStringList &suffixes) const;

    // Extraction. Paths are sanitised per entry and written relative to dest,
    // without the prefix folder, so a caller that wants the engine's mount path
    // passes dest + "/" + prefix(). Returns false only when the extract could
    // not start; individual refusals land in the report and leave the rest of
    // the archive extracted.
    bool extract(const QString &destDir, const QStringList &suffixes,
                 PboExtractReport *report = nullptr, QString *error = nullptr) const;
    bool extractAll(const QString &destDir, PboExtractReport *report = nullptr,
                    QString *error = nullptr) const;
    // Only the files this app can read back as something other than bytes.
    bool extractScripts(const QString &destDir, PboExtractReport *report = nullptr,
                        QString *error = nullptr) const;

private:
    bool extractOne(const PboEntry &entry, const QString &destDir,
                    PboExtractReport *report) const;

    QString m_path;
    mutable QFile m_file;
    qint64 m_size = 0;
    QString m_prefix;
    QHash<QString, QString> m_headers;
    QVector<PboEntry> m_entries;
    QHash<QString, int> m_byName;   // normalised name to index, for find()
    bool m_reconciles = false;
};

// .c, .cpp, .xml, .json, .csv, .layout
const QStringList &pboReadableSuffixes();

// The LZSS the format packs with. Exposed so the decoder can be tested without
// an archive, and so a caller holding the raw bytes can decode them itself.
//
// Empty with an error set means refused. originalSize of zero yields an empty
// result and no error: obfuscated archives carry thousands of entries shaped
// that way and there is nothing in them to produce.
QByteArray pboDecompress(const QByteArray &packed, quint32 originalSize,
                         QString *error = nullptr);

// An entry name turned into a relative path that is safe to join onto a folder,
// or empty with the reason set. This is the check that stops a hostile archive
// from writing outside the folder it was told to write to, so it refuses rather
// than repairs: `..` in any segment, a leading slash, a drive letter, a UNC
// root, control characters, the characters Windows reserves, the device names
// Windows reserves, and any segment Windows would silently rename by stripping
// a trailing dot or space.
QString pboSafeRelativePath(const QString &entryName, QString *reason = nullptr);

// Every .pbo under a folder. Mods under !Workshop are directory junctions, so
// the walk resolves links, and it remembers where it has been so a link that
// points at its own parent cannot spin.
QStringList pbosUnder(const QString &root, int maxDepth = 4);

// BankRev.exe, the fallback for an archive this refuses. Empty when DayZ Tools
// is not installed. The DAYZ_TOOLS_BIN environment variable wins when it is set,
// because the registry key DayZ Tools is supposed to write is not set on every
// machine that has the tools.
QString pboBankRevPath();
bool pboExtractWithBankRev(const QString &pboPath, const QString &destDir,
                           QString *error = nullptr, int timeoutMs = 120000);
