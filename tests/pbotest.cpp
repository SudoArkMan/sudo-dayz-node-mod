// Reading a mod's .pbo archives without DayZ Tools.
//
// Four things have to hold, and each one is a way the mod browser breaks in the
// user's hands rather than in a build:
//
//   header      an archive somebody uploaded has to be parsed or refused, never
//               trusted. A bogus entry count, a size that runs past the end and
//               a name carrying `..` all appear in the mods installed on this
//               machine, because three of them went through an obfuscator.
//   LZSS        the decoder has to be exactly right. The checksum trailing each
//               packed entry is the proof, so it is checked every time and a
//               mismatch refuses the entry instead of handing back plausible
//               noise. BankRev.exe is the outside oracle when it is installed.
//   extraction  every path is sanitised before anything is opened. Path
//               traversal on extract is the one failure here that damages a
//               machine rather than a session.
//   the corpus  254 mods are installed on this machine and they are the reason
//               this exists, so the rate over all of them is printed as it is.
//
// The synthetic half builds its archives in a QTemporaryDir and needs no mod
// installed. The corpus half skips itself when the workshop folder is absent:
//   ./tests/pbotest ../resources [--limit N] [--workshop <dir>]

#include "pbo/pboreader.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMap>
#include <QTemporaryDir>
#include <QTextStream>

static int fails = 0;

static void check(bool ok, const QString &what)
{
    QTextStream o(stdout);
    o << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) fails++;
}

static void heading(const QString &what)
{
    QTextStream o(stdout);
    o << Qt::endl << what << Qt::endl;
}

static QString percent(qint64 part, qint64 whole)
{
    if (whole <= 0) return QStringLiteral("n/a");
    return QStringLiteral("%1 %").arg(100.0 * double(part) / double(whole), 0, 'f', 1);
}

// A failure reason carries the name and the size that caused it, which is what
// makes it useful on its own and useless as a grouping key. Numbers become N and
// quoted names become '..' so the groups count causes rather than instances.
static QString generalise(const QString &reason)
{
    QString out;
    bool inQuote = false;
    bool lastWasDigit = false;
    for (const QChar ch : reason) {
        if (ch == QLatin1Char('\'')) {
            if (!inQuote) out += QStringLiteral("'..'");
            inQuote = !inQuote;
            continue;
        }
        if (inQuote) continue;
        if (ch.isDigit()) {
            if (!lastWasDigit) out += QLatin1Char('N');
            lastWasDigit = true;
            continue;
        }
        lastWasDigit = false;
        out += ch;
    }
    return out;
}

// ---------------------------------------------------------------- synthetic

static QByteArray u32(quint32 v)
{
    QByteArray out(4, '\0');
    for (int i = 0; i < 4; ++i) out[i] = char((v >> (8 * i)) & 0xFF);
    return out;
}

struct MadeEntry {
    QString name;
    QByteArray data;             // the bytes as they sit in the archive
    quint32 mime = 0;            // kPboMimeCompressed for a packed entry
    quint32 originalSize = 0;    // 0 means the entry is stored, so data is it
};

// An archive built the way the format says, so the reader is tested against
// bytes rather than against itself.
static QByteArray makeArchive(const QString &prefix, const QVector<MadeEntry> &entries)
{
    QByteArray out;
    out += '\0';
    out += u32(kPboMimeProduct) + u32(0) + u32(0) + u32(0) + u32(0);
    out += QByteArray("prefix") + '\0' + prefix.toLatin1() + '\0';
    out += QByteArray("Mikero") + '\0' + QByteArray("test") + '\0';
    out += '\0';  // empty key ends the product header
    for (const MadeEntry &entry : entries) {
        const quint32 original = entry.originalSize > 0 ? entry.originalSize
                                                        : quint32(entry.data.size());
        out += entry.name.toLatin1();
        out += '\0';
        out += u32(entry.mime) + u32(original) + u32(0) + u32(0)
            + u32(quint32(entry.data.size()));
    }
    out += '\0';
    out += u32(0) + u32(0) + u32(0) + u32(0) + u32(0);
    for (const MadeEntry &entry : entries) out += entry.data;
    out += '\0';
    out += QCryptographicHash::hash(out, QCryptographicHash::Sha1);
    return out;
}

static bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    return f.write(bytes) == bytes.size();
}

static void testDecompressor()
{
    heading(QStringLiteral("LZSS, on streams built by hand"));

    // One literal 'a', then a match reaching one byte back for seven more.
    // The four trailing bytes are the sum of the decoded bytes, 8 * 97.
    const QByteArray runOfA = QByteArray::fromRawData("\x01" "a\x01\x04\x08\x03\x00\x00", 8);
    QString why;
    QByteArray got = pboDecompress(runOfA, 8, &why);
    check(why.isEmpty() && got == QByteArray("aaaaaaaa"),
          QStringLiteral("a match reaching back into the output repeats it"));

    // A match at the very start, reaching three bytes behind the output. The
    // encoder's window starts full of spaces, so that is what comes back.
    const QByteArray beforeStart = QByteArray::fromRawData("\x00\x03\x01\x80\x00\x00\x00", 7);
    got = pboDecompress(beforeStart, 4, &why);
    check(why.isEmpty() && got == QByteArray("    "),
          QStringLiteral("a match reaching behind the start of the output gives spaces"));

    QByteArray wrongSum = runOfA;
    wrongSum[4] = char(0x09);
    got = pboDecompress(wrongSum, 8, &why);
    check(got.isEmpty() && why.contains(QLatin1String("checksum")),
          QStringLiteral("a checksum that disagrees refuses the entry"));

    got = pboDecompress(runOfA, 100000000, &why);
    check(got.isEmpty() && !why.isEmpty(),
          QStringLiteral("an original size LZSS could not have produced is refused"));

    got = pboDecompress(QByteArray("\x01" "a", 2), 64, &why);
    check(got.isEmpty() && why.contains(QLatin1String("ends mid stream")),
          QStringLiteral("packed data that stops early is refused"));

    got = pboDecompress(QByteArray(), 0, &why);
    check(got.isEmpty() && why.isEmpty(),
          QStringLiteral("an entry of nothing decodes to nothing without an error"));
}

static void testSanitiser()
{
    heading(QStringLiteral("Entry names, before anything is opened"));

    const auto refused = [](const QString &name) {
        QString reason;
        return pboSafeRelativePath(name, &reason).isEmpty() && !reason.isEmpty();
    };

    check(pboSafeRelativePath(QStringLiteral("Scripts\\4_World\\Thing.c"))
              == QStringLiteral("Scripts/4_World/Thing.c"),
          QStringLiteral("a normal entry name becomes a relative path"));
    check(refused(QStringLiteral("..\\..\\evil.txt")), QStringLiteral("`..` is refused"));
    check(refused(QStringLiteral("a\\..\\..\\b.c")),
          QStringLiteral("`..` in the middle of a path is refused"));
    check(refused(QStringLiteral("C:\\Windows\\evil.txt")),
          QStringLiteral("a drive letter is refused"));
    check(refused(QStringLiteral("/etc/passwd")), QStringLiteral("a rooted path is refused"));
    check(refused(QStringLiteral("\\\\server\\share\\x.c")),
          QStringLiteral("a UNC root is refused"));
    check(refused(QStringLiteral("con.txt")),
          QStringLiteral("a Windows device name is refused"));
    check(refused(QStringLiteral("Scripts\\NUL")),
          QStringLiteral("a device name in a subfolder is refused"));
    check(refused(QStringLiteral("Scripts\\a\x01" "b.c")),
          QStringLiteral("a control character is refused"));
    check(refused(QStringLiteral("Scripts \\thing.c")),
          QStringLiteral("a segment Windows would rename by stripping a space is refused"));
    check(refused(QStringLiteral("Scripts.\\thing.c")),
          QStringLiteral("a segment ending in a dot is refused"));
    check(refused(QStringLiteral("a\\b<c>.c")),
          QStringLiteral("a character Windows reserves is refused"));
    check(refused(QString()), QStringLiteral("an empty name is refused"));
}

// The stream from the decompressor tests: one literal 'a' and a match that
// repeats it seven times, then the checksum.
static const QByteArray kPackedRunOfA = QByteArray::fromRawData("\x01" "a\x01\x04\x08\x03\x00\x00", 8);

static void testSyntheticArchive(const QString &work)
{
    heading(QStringLiteral("An archive read back"));

    QVector<MadeEntry> entries;
    entries << MadeEntry{QStringLiteral("config.cpp"), QByteArray("class CfgPatches {};\n")};
    entries << MadeEntry{QStringLiteral("Scripts\\4_World\\Thing.c"),
                         QByteArray("modded class PlayerBase {}\n")};
    entries << MadeEntry{QStringLiteral("data\\model.p3d"), QByteArray(4096, '\x7f')};
    entries << MadeEntry{QStringLiteral("empty.xml"), QByteArray()};
    entries << MadeEntry{QStringLiteral("Scripts\\Packed.c"), kPackedRunOfA,
                         kPboMimeCompressed, 8};

    const QString pboPath = work + QStringLiteral("/good.pbo");
    check(writeFile(pboPath, makeArchive(QStringLiteral("TestMod_Scripts"), entries)),
          QStringLiteral("the archive under test was written"));

    PboFile pbo;
    QString error;
    const bool opened = pbo.open(pboPath, &error);
    check(opened, QStringLiteral("it opens%1")
                      .arg(error.isEmpty() ? QString() : QStringLiteral(": ") + error));
    check(pbo.prefix() == QStringLiteral("TestMod_Scripts"),
          QStringLiteral("the prefix comes off the product entry"));
    check(pbo.headers().value(QStringLiteral("Mikero")) == QStringLiteral("test"),
          QStringLiteral("every header key/value is kept"));
    check(pbo.entries().size() == 5, QStringLiteral("every entry is listed"));
    check(pbo.reconciles(), QStringLiteral("header, data and trailer add up to the file"));

    check(pbo.read(QStringLiteral("config.cpp")) == entries.at(0).data,
          QStringLiteral("a stored entry reads back byte for byte"));
    check(pbo.read(QStringLiteral("Scripts/4_World/Thing.c")) == entries.at(1).data,
          QStringLiteral("a forward slash finds an entry written with backslashes"));
    check(pbo.read(QStringLiteral("DATA\\MODEL.P3D")) == entries.at(2).data,
          QStringLiteral("lookup ignores case"));
    check(pbo.read(QStringLiteral("empty.xml")).isEmpty(),
          QStringLiteral("an empty entry reads back empty"));
    check(pbo.read(QStringLiteral("Scripts\\Packed.c")) == QByteArray("aaaaaaaa"),
          QStringLiteral("a packed entry is unpacked on the way out"));
    const PboEntry *packed = pbo.find(QStringLiteral("Scripts\\Packed.c"));
    check(packed && packed->compressed() && packed->originalSize == 8 && packed->dataSize == 8,
          QStringLiteral("a packed entry reports both of its sizes"));
    error.clear();
    check(pbo.read(QStringLiteral("nope.c"), &error).isEmpty() && !error.isEmpty(),
          QStringLiteral("an entry that is not there says so"));

    const QStringList scripts = pbo.filesMatching(QStringLiteral(".c"));
    check(scripts.size() == 2 && scripts.first().endsWith(QStringLiteral("Thing.c")),
          QStringLiteral("filesMatching('.c') finds the scripts and not config.cpp"));
    check(pbo.filesMatching(QStringList{QStringLiteral(".cpp"), QStringLiteral(".xml")}).size() == 2,
          QStringLiteral("filesMatching takes a list of suffixes"));

    // Offsets have to be where the format says, not where a whole-file scan
    // would find them, because a 200 MB pbo is never going to be read whole.
    const PboEntry *thing = pbo.find(QStringLiteral("Scripts\\4_World\\Thing.c"));
    check(thing && thing->offset > 0 && thing->offset + thing->dataSize <= pbo.fileSize(),
          QStringLiteral("an entry knows where its bytes start"));

    const QString dest = work + QStringLiteral("/extract-good");
    PboExtractReport report;
    check(pbo.extractScripts(dest, &report, &error), QStringLiteral("extractScripts runs"));
    check(report.written == 4 && report.refused == 0,
          QStringLiteral("it writes the .cpp, both .c files and the .xml, and refuses none"));
    check(QFile::exists(dest + QStringLiteral("/Scripts/4_World/Thing.c")),
          QStringLiteral("the script lands at its path inside the archive"));
    check(!QFile::exists(dest + QStringLiteral("/data/model.p3d")),
          QStringLiteral("the model is left behind, it is not one this app reads"));

    const QString allDest = work + QStringLiteral("/extract-all");
    PboExtractReport allReport;
    pbo.extractAll(allDest, &allReport, &error);
    check(allReport.written == 5, QStringLiteral("extractAll writes every entry"));
    QFile written(allDest + QStringLiteral("/data/model.p3d"));
    check(written.open(QIODevice::ReadOnly) && written.readAll() == entries.at(2).data,
          QStringLiteral("a stored entry is written back byte for byte"));
    QFile unpacked(allDest + QStringLiteral("/Scripts/Packed.c"));
    check(unpacked.open(QIODevice::ReadOnly) && unpacked.readAll() == QByteArray("aaaaaaaa"),
          QStringLiteral("a packed entry is written out unpacked"));

    // A packed entry whose bytes were tampered with has to be refused, not
    // written half decoded, and the good entries around it still come out.
    QVector<MadeEntry> tampered = entries;
    QByteArray broken = kPackedRunOfA;
    broken[4] = char(0x09);
    tampered[4] = MadeEntry{QStringLiteral("Scripts\\Packed.c"), broken, kPboMimeCompressed, 8};
    const QString tamperedPath = work + QStringLiteral("/tampered.pbo");
    writeFile(tamperedPath, makeArchive(QStringLiteral("TestMod_Scripts"), tampered));
    PboFile tamperedPbo;
    tamperedPbo.open(tamperedPath, &error);
    PboExtractReport tamperedReport;
    tamperedPbo.extractAll(work + QStringLiteral("/extract-tampered"), &tamperedReport, &error);
    check(tamperedReport.written == 4 && tamperedReport.refused == 1,
          QStringLiteral("a packed entry that fails its checksum is refused, the rest are "
                         "written (%1 written, %2 refused)")
              .arg(tamperedReport.written).arg(tamperedReport.refused));
    check(!QFile::exists(work + QStringLiteral("/extract-tampered/Scripts/Packed.c")),
          QStringLiteral("nothing half decoded is left on disk"));
}

static void testDuplicateNames(const QString &work)
{
    heading(QStringLiteral("The same name more than once"));

    QVector<MadeEntry> entries;
    entries << MadeEntry{QStringLiteral("Scripts\\Thing.c"), QByteArray("first")};
    entries << MadeEntry{QStringLiteral("scripts\\thing.c"), QByteArray("second")};
    const QString pboPath = work + QStringLiteral("/dupes.pbo");
    writeFile(pboPath, makeArchive(QStringLiteral("Dupes"), entries));

    PboFile pbo;
    QString error;
    check(pbo.open(pboPath, &error), QStringLiteral("an archive with repeated names opens"));
    check(pbo.entries().size() == 2, QStringLiteral("both copies are listed"));
    check(pbo.read(QStringLiteral("Scripts/Thing.c")) == QByteArray("first"),
          QStringLiteral("the first copy wins the lookup"));

    // Only one file can exist per path, so the extract writes one of them and
    // says so rather than pretending both landed.
    const QString dest = work + QStringLiteral("/extract-dupes");
    PboExtractReport report;
    pbo.extractAll(dest, &report, &error);
    check(report.written == 2 && report.refused == 0,
          QStringLiteral("both are written, the second over the first"));
    QFile onDisk(dest + QStringLiteral("/Scripts/Thing.c"));
    check(onDisk.open(QIODevice::ReadOnly) && onDisk.readAll() == QByteArray("second"),
          QStringLiteral("the file on disk holds the last copy written"));
}

static void testHostileArchive(const QString &work)
{
    heading(QStringLiteral("Archives built to break the extract"));

    QVector<MadeEntry> entries;
    entries << MadeEntry{QStringLiteral("..\\..\\owned.txt"), QByteArray("no")};
    entries << MadeEntry{QStringLiteral("C:\\Windows\\owned.txt"), QByteArray("no")};
    entries << MadeEntry{QStringLiteral("/etc/passwd"), QByteArray("no")};
    entries << MadeEntry{QStringLiteral("con.txt"), QByteArray("no")};
    entries << MadeEntry{QStringLiteral("Scripts \\owned.c"), QByteArray("no")};
    entries << MadeEntry{QStringLiteral("Scripts\\ok.c"), QByteArray("yes")};

    const QString pboPath = work + QStringLiteral("/hostile.pbo");
    writeFile(pboPath, makeArchive(QStringLiteral("Hostile"), entries));

    PboFile pbo;
    QString error;
    check(pbo.open(pboPath, &error),
          QStringLiteral("an archive with hostile names still opens, the names are the problem"));

    const QString sandbox = work + QStringLiteral("/sandbox");
    QDir().mkpath(sandbox);
    const QString dest = sandbox + QStringLiteral("/dest");
    PboExtractReport report;
    pbo.extractAll(dest, &report, &error);
    check(report.written == 1 && report.refused == 5,
          QStringLiteral("one entry is written and five are refused, got %1 and %2")
              .arg(report.written).arg(report.refused));
    check(QFile::exists(dest + QStringLiteral("/Scripts/ok.c")),
          QStringLiteral("the one safe entry is written"));
    check(!QFile::exists(sandbox + QStringLiteral("/owned.txt"))
              && !QFile::exists(work + QStringLiteral("/owned.txt")),
          QStringLiteral("nothing landed above the destination folder"));
    check(report.refusals.size() == 5,
          QStringLiteral("each refusal carries the name and the reason"));

    // A header that claims an entry longer than the file it lives in.
    QByteArray truncated = makeArchive(QStringLiteral("Short"),
                                       {MadeEntry{QStringLiteral("a.c"), QByteArray("hello")}});
    const int sizeField = truncated.indexOf(QByteArray("a.c")) + 4 + 16;
    truncated.replace(sizeField, 4, u32(0x7FFFFFFF));
    writeFile(work + QStringLiteral("/toobig.pbo"), truncated);
    // The order the arguments to check() are evaluated in is up to the
    // compiler, so the open has to happen before the message is built or the
    // message reports the error from the call before it.
    PboFile bad;
    error.clear();
    bool refusedIt = !bad.open(work + QStringLiteral("/toobig.pbo"), &error);
    check(refusedIt && error.contains(QLatin1String("past the end")),
          QStringLiteral("an entry running past the end of the file is refused: %1").arg(error));

    // A header with no terminating entry at all.
    QByteArray runaway = makeArchive(QStringLiteral("Runaway"),
                                     {MadeEntry{QStringLiteral("a.c"), QByteArray("hello")}});
    runaway = runaway.left(runaway.indexOf(QByteArray("a.c")) + 3);
    writeFile(work + QStringLiteral("/runaway.pbo"), runaway);
    error.clear();
    refusedIt = !bad.open(work + QStringLiteral("/runaway.pbo"), &error);
    check(refusedIt && !error.isEmpty(),
          QStringLiteral("a header with no terminator is refused: %1").arg(error));

    writeFile(work + QStringLiteral("/tiny.pbo"), QByteArray(8, '\0'));
    error.clear();
    refusedIt = !bad.open(work + QStringLiteral("/tiny.pbo"), &error);
    check(refusedIt && !error.isEmpty(),
          QStringLiteral("a file too small to be an archive is refused"));

    error.clear();
    refusedIt = !bad.open(work + QStringLiteral("/not-here.pbo"), &error);
    check(refusedIt && !error.isEmpty(),
          QStringLiteral("a file that is not there is refused"));

    // Half a million entries claimed by a file with room for none of them.
    QByteArray absurd;
    absurd += '\0';
    absurd += u32(kPboMimeProduct) + u32(0) + u32(0) + u32(0) + u32(0);
    absurd += '\0';
    for (int i = 0; i < 64; ++i) {
        absurd += QStringLiteral("f%1.c").arg(i).toLatin1();
        absurd += '\0';
        absurd += u32(0) + u32(1 << 20) + u32(0) + u32(0) + u32(1 << 20);
    }
    absurd += '\0';
    absurd += u32(0) + u32(0) + u32(0) + u32(0) + u32(0);
    absurd += QByteArray(21, '\0');
    writeFile(work + QStringLiteral("/absurd.pbo"), absurd);
    error.clear();
    check(!bad.open(work + QStringLiteral("/absurd.pbo"), &error) && !error.isEmpty(),
          QStringLiteral("entries claiming more bytes than the file holds are refused"));
}

// ------------------------------------------------------------------- corpus

struct Sweep {
    int pbos = 0;
    int opened = 0;
    int withPrefix = 0;
    int reconciled = 0;
    int obfuscated = 0;
    qint64 entries = 0;
    qint64 compressed = 0;
    qint64 stored = 0;
    qint64 decoded = 0;
    qint64 decodeRefused = 0;
    qint64 bytesOut = 0;
    QMap<QString, int> openFailures;
    QMap<QString, int> decodeFailures;
    QStringList openFailureExamples;
};

// Every packed entry is decoded, not a sample of them. The checksum on each one
// is the only proof the decoder is right, and decoding all 9.3 million of them
// costs about half a minute, so there is nothing to buy by checking fewer.
static void sweepCorpus(const QString &root, int limit, Sweep *sweep)
{
    QStringList pbos = pbosUnder(root);
    if (limit > 0 && pbos.size() > limit) pbos = pbos.mid(0, limit);
    sweep->pbos = pbos.size();

    for (const QString &path : pbos) {
        PboFile pbo;
        QString error;
        if (!pbo.open(path, &error)) {
            const QString key = generalise(error);
            sweep->openFailures[key]++;
            if (sweep->openFailureExamples.size() < 8)
                sweep->openFailureExamples << (QFileInfo(path).fileName()
                                               + QStringLiteral(": ") + error);
            continue;
        }
        sweep->opened++;
        if (!pbo.prefix().isEmpty()) sweep->withPrefix++;
        if (pbo.reconciles()) sweep->reconciled++;
        if (pbo.headers().contains(QStringLiteral("obfuscated"))) sweep->obfuscated++;
        sweep->entries += pbo.entries().size();
        for (const PboEntry &entry : pbo.entries()) {
            if (!entry.compressed()) { sweep->stored++; continue; }
            sweep->compressed++;
            QString why;
            const QByteArray bytes = pbo.read(entry, &why);
            if (!why.isEmpty()) {
                sweep->decodeRefused++;
                sweep->decodeFailures[generalise(why)]++;
                continue;
            }
            sweep->decoded++;
            sweep->bytesOut += bytes.size();
        }
    }
}

// BankRev.exe is the only outside oracle for the decoder. Everything else in
// this test is this code checking itself.
static void compareAgainstBankRev(const QString &root, const QString &work)
{
    heading(QStringLiteral("Against BankRev.exe, the outside oracle"));

    const QString exe = pboBankRevPath();
    if (exe.isEmpty()) {
        QTextStream(stdout) << "  skip   BankRev.exe is not on this machine" << Qt::endl;
        return;
    }
    QTextStream(stdout) << "  using  " << exe << Qt::endl;

    // Small archives that carry packed script, which is what this reader exists
    // to pull out. The entry count is capped as well as the size: a packer that
    // writes 23,000 entries into a 2 MB archive takes BankRev minutes to unpack
    // and proves no more than a small one does. More candidates are gathered
    // than are needed, because BankRev refuses some of them outright.
    QStringList candidates;
    for (const QString &path : pbosUnder(root)) {
        const QFileInfo info(path);
        if (info.size() > (8LL << 20)) continue;
        PboFile probe;
        if (!probe.open(path)) continue;
        if (probe.entries().size() > 5000) continue;
        int packedScripts = 0;
        for (const PboEntry &entry : probe.entries()) {
            if (entry.compressed() && entry.originalSize > 0
                && entry.name.endsWith(QStringLiteral(".c"), Qt::CaseInsensitive))
                packedScripts++;
        }
        if (packedScripts >= 8) candidates << path;
        if (candidates.size() >= 60) break;
    }
    if (candidates.isEmpty()) {
        QTextStream(stdout) << "  skip   no small archive with packed script to compare"
                            << Qt::endl;
        return;
    }

    QTextStream out(stdout);
    int same = 0, differ = 0, missing = 0, compared = 0, bankRevRefused = 0;
    for (const QString &path : candidates) {
        if (compared >= 12) break;
        const QString base = QFileInfo(path).completeBaseName();
        const QString dest = work + QStringLiteral("/oracle/") + base;
        QString error;
        if (!pboExtractWithBankRev(path, dest, &error)) {
            // Worth printing rather than hiding: BankRev turns down every
            // archive that went through an obfuscator, and this reader does not.
            bankRevRefused++;
            out << QStringLiteral("  %1: BankRev would not read it (%2), this reader did")
                       .arg(base).arg(error)
                << Qt::endl;
            continue;
        }
        compared++;
        // BankRev writes into <dest>/<archive name>/ and lowercases every path.
        const QString extracted = dest + QLatin1Char('/') + base;
        PboFile pbo;
        if (!pbo.open(path, &error)) { differ++; continue; }
        // A packed archive carries the same name many times over, and only one
        // of them can be the file on disk. Comparing every entry against that
        // one file would report the other copies as wrong, so only names that
        // appear once in the archive are compared.
        QHash<QString, int> timesSeen;
        for (const PboEntry &entry : pbo.entries()) {
            const QString relative = pboSafeRelativePath(entry.name);
            if (!relative.isEmpty()) timesSeen[relative.toLower()]++;
        }

        int mine = 0, mismatch = 0, absent = 0, ambiguous = 0;
        for (const PboEntry &entry : pbo.entries()) {
            const QString relative = pboSafeRelativePath(entry.name);
            if (relative.isEmpty()) continue;
            if (timesSeen.value(relative.toLower()) != 1) { ambiguous++; continue; }
            QFile oracle(extracted + QLatin1Char('/') + relative.toLower());
            if (!oracle.open(QIODevice::ReadOnly)) { absent++; continue; }
            const QByteArray want = oracle.readAll();
            QString why;
            const QByteArray got = pbo.read(entry, &why);
            if (why.isEmpty() && got == want) {
                mine++;
            } else {
                mismatch++;
                if (mismatch <= 3)
                    out << QStringLiteral("    differs: %1 (%2 bytes wanted, %3 decoded%4)")
                               .arg(entry.name).arg(want.size()).arg(got.size())
                               .arg(why.isEmpty() ? QString()
                                                  : QStringLiteral(", refused: ") + why)
                        << Qt::endl;
            }
        }
        if (ambiguous > 0)
            out << QStringLiteral("    %1 entries share a name with another, skipped")
                       .arg(ambiguous)
                << Qt::endl;
        same += mine;
        differ += mismatch;
        missing += absent;
        out << QStringLiteral("  %1: %2 identical, %3 differed, %4 BankRev did not write")
                   .arg(base).arg(mine).arg(mismatch).arg(absent)
            << Qt::endl;
    }
    out << QStringLiteral("  %1 entries byte for byte, %2 differed, %3 not written by BankRev, "
                          "over %4 archives (%5 BankRev refused)")
               .arg(same).arg(differ).arg(missing).arg(compared).arg(bankRevRefused)
        << Qt::endl;
    check(same > 0 && differ == 0,
          QStringLiteral("every entry BankRev extracted decodes to the same bytes"));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    int limit = 0;
    QString workshop = QStringLiteral("D:/SteamLibrary/steamapps/common/DayZ/!Workshop");
    const QStringList args = app.arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("--limit") && i + 1 < args.size())
            limit = args.at(++i).toInt();
        else if (args.at(i) == QStringLiteral("--workshop") && i + 1 < args.size())
            workshop = args.at(++i);
    }

    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        out << "cannot make a temporary folder" << Qt::endl;
        return 1;
    }

    testDecompressor();
    testSanitiser();
    testSyntheticArchive(tmp.path());
    testDuplicateNames(tmp.path());
    testHostileArchive(tmp.path());

    heading(QStringLiteral("The installed mods"));
    if (!QFileInfo(workshop).isDir()) {
        out << "  skip   no workshop folder at " << workshop << Qt::endl;
    } else {
        const QFileInfoList mods = QDir(workshop).entryInfoList(
            QStringList{QStringLiteral("@*")}, QDir::Dirs | QDir::NoDotAndDotDot);
        Sweep sweep;
        QElapsedTimer timer;
        timer.start();
        sweepCorpus(workshop, limit, &sweep);
        const double seconds = double(timer.elapsed()) / 1000.0;

        out << "  " << workshop << Qt::endl;
        out << QStringLiteral("  mods                 %1").arg(mods.size(), 12) << Qt::endl;
        out << QStringLiteral("  pbo files            %1").arg(sweep.pbos, 12) << Qt::endl;
        out << QStringLiteral("  opened               %1   %2")
                   .arg(sweep.opened, 12).arg(percent(sweep.opened, sweep.pbos))
            << Qt::endl;
        out << QStringLiteral("  carried a prefix     %1   %2")
                   .arg(sweep.withPrefix, 12).arg(percent(sweep.withPrefix, sweep.opened))
            << Qt::endl;
        out << QStringLiteral("  sizes reconciled     %1   %2")
                   .arg(sweep.reconciled, 12).arg(percent(sweep.reconciled, sweep.opened))
            << Qt::endl;
        out << QStringLiteral("  ran through a packer %1   (BankRev.exe turns these down)")
                   .arg(sweep.obfuscated, 12)
            << Qt::endl;
        out << QStringLiteral("  entries              %1").arg(sweep.entries, 12) << Qt::endl;
        out << QStringLiteral("  packed               %1   %2")
                   .arg(sweep.compressed, 12).arg(percent(sweep.compressed, sweep.entries))
            << Qt::endl;
        out << QStringLiteral("  stored               %1   %2")
                   .arg(sweep.stored, 12).arg(percent(sweep.stored, sweep.entries))
            << Qt::endl;
        out << QStringLiteral("  decoded and checked  %1   %2")
                   .arg(sweep.decoded, 12)
                   .arg(percent(sweep.decoded, sweep.decoded + sweep.decodeRefused))
            << Qt::endl;
        out << QStringLiteral("  decode refused       %1").arg(sweep.decodeRefused, 12)
            << Qt::endl;
        out << QStringLiteral("  bytes produced       %1 MB in %2 s")
                   .arg(double(sweep.bytesOut) / 1048576.0, 12, 'f', 1).arg(seconds, 0, 'f', 1)
            << Qt::endl;

        if (!sweep.openFailures.isEmpty()) {
            out << Qt::endl << "  archives refused, by reason:" << Qt::endl;
            for (auto it = sweep.openFailures.constBegin(); it != sweep.openFailures.constEnd();
                 ++it)
                out << QStringLiteral("    %1  %2").arg(it.value(), 6).arg(it.key()) << Qt::endl;
            for (const QString &example : sweep.openFailureExamples)
                out << QStringLiteral("      e.g. %1").arg(example) << Qt::endl;
        }
        if (!sweep.decodeFailures.isEmpty()) {
            out << Qt::endl << "  entries refused, by reason:" << Qt::endl;
            for (auto it = sweep.decodeFailures.constBegin();
                 it != sweep.decodeFailures.constEnd(); ++it)
                out << QStringLiteral("    %1  %2").arg(it.value(), 6).arg(it.key()) << Qt::endl;
        }

        out << Qt::endl;
        check(sweep.pbos > 0, QStringLiteral("the workshop folder held archives to read"));
        check(sweep.opened == sweep.pbos,
              QStringLiteral("every installed archive opened (%1 of %2)")
                  .arg(sweep.opened).arg(sweep.pbos));
        check(sweep.decodeRefused == 0,
              QStringLiteral("every packed entry decoded and matched its checksum (%1 refused)")
                  .arg(sweep.decodeRefused));

        compareAgainstBankRev(workshop, tmp.path());
    }

    out << Qt::endl << (fails == 0 ? QStringLiteral("all checks passed")
                                   : QStringLiteral("%1 check(s) failed").arg(fails))
        << Qt::endl;
    return fails == 0 ? 0 : 1;
}
