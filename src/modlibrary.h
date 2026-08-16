// The installed mods, as a library you can open and read.
//
// Two halves, and they are deliberately different shapes.
//
// Scanning is cheap and wide: walk the mod roots, read mod.cpp and meta.cpp for
// a name and an author, and read each pbo's header for its prefix and its
// script count. 254 mods is enough that this must not happen on every open, so
// the result is cached under AppDataLocation keyed by folder and modified time,
// and a refresh runs on a worker thread instead of on the UI.
//
// Opening is narrow and expensive: extract one mod's .c files and run the
// importer over each. That part stays on the calling thread on purpose. The
// importer resolves pins through Catalog, whose def cache is mutable, so
// running it beside a UI that is also drawing from the same Catalog would race.
// openModStep exists so a caller can spend that cost a few files at a time and
// keep painting between them.
//
// Read only, without exception. Somebody else's mod is theirs: nothing here
// writes inside a mod folder, every extracted file lands under the app's own
// cache, and every graph that comes out carries a flag saying it may not be
// saved back.
#pragma once

#include "graph.h"

#include <QHash>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

class Builtins;
class Catalog;
class QThread;
struct Project;
struct ScriptEntry;

// ------------------------------------------------------------------ the scan

// One pbo inside a mod, as its header describes it. Nothing here reads file
// data: the header alone answers what the engine mounts it as and whether it is
// worth opening.
struct ModPbo {
    QString path;          // absolute
    QString prefix;        // mount prefix from the header, "Project3DPrinter_Scripts"
    qint64 size = 0;
    int entryCount = 0;
    int scriptCount = 0;   // entries ending in .c
    bool readable = false; // header parsed; false means the reader refused it
    QString error;         // why it was refused, for the row's tooltip
};

// An installed mod. Everything here is either a directory fact or something
// mod.cpp / meta.cpp said, so the whole record is producible without opening a
// pbo's data section.
struct ModEntry {
    QString folder;        // absolute path of the @Mod folder
    QString folderName;    // "@3D Printer"
    QString name;          // mod.cpp `name`, else meta.cpp, else the folder name
    // Only 77 of the 254 installed mods ship a mod.cpp at all, so for most of
    // them the only name attached to the work is the key that signed the pbo.
    // That is worth showing, and worth flagging as what it is.
    QString author;
    bool authorIsSigner = false;
    QString version;
    QString picture;       // logo path as written, for a caller that can load .paa
    QString overview;      // mod.cpp `tooltip` or `overview`, one line
    QString publishedId;   // meta.cpp `publishedid`, empty for a local mod
    QVector<ModPbo> pbos;
    // Scripts already sitting on disk rather than packed into a pbo: a mod
    // somebody has unpacked, or a bare Scripts folder. Absolute paths, and only
    // ever filled by readModPath below. The scan never produces one, because a
    // folder of loose .c files under a mod root is not a mod anyone installed.
    QStringList looseScripts;
    qint64 modified = 0;   // newest mtime under the folder, ms since epoch
    QStringList notes;     // what the scan could not read, per mod

    int scriptCount() const;
    bool hasScripts() const { return scriptCount() > 0; }
    bool isValid() const { return !folder.isEmpty(); }
};

// -------------------------------------------------------- a path from disk
//
// The library scans known roots, which answers "what is installed" and nothing
// else. A mod handed over on a stick, a pbo pulled out of a workshop download,
// or a folder somebody has already unpacked are all things a person wants to
// read, and none of them are under a root. These turn any such path into the
// same ModEntry the scan produces, so opening it costs the same code.

enum class ModPathKind {
    None,          // not there, or nothing in it this can read
    Pbo,           // one .pbo
    ModFolder,     // a mod: pbos directly under it or under its Addons folder
    ScriptFolder,  // .c files on disk, a mod somebody has unpacked
    ModRoot,       // a folder of mods, which is a root to scan rather than a mod
};

ModPathKind classifyModPath(const QString &path);

// A mod record for a path the scan did not turn up. A ModRoot comes back
// invalid with the reason set, because a folder holding mods is added as a root
// rather than opened as one mod.
ModEntry readModPath(const QString &path, QString *error = nullptr);

// --------------------------------------------------------------- opening one

struct ModOpenOptions {
    // Capped by default, and 0 turns a cap off. The safe value is the default
    // because the corpus decides this, not the caller: most mods ship a handful
    // of scripts, and three of them are run through an obfuscator that pads the
    // entry table out to six figures.
    int maxFiles = 400;
    qint64 maxBytes = 32 * 1024 * 1024;
    // Skip files bigger than this. A 400 KB generated .c is not a graph anyone
    // wants to look at, and it costs seconds to lay out.
    qint64 maxFileBytes = 512 * 1024;
};

// One .c pulled out of a pbo. `path` always sits under scriptCacheRoot().
struct ModScriptFile {
    QString pbo;       // file name of the pbo it came from
    QString entry;     // path inside that pbo, as the header spelled it
    QString path;      // where it was extracted to
    qint64 size = 0;
};

// One class the importer got out of a file.
struct ModClassView {
    QString className;
    QString baseClass;
    bool modded = false;
    QString pbo;
    QString entry;     // path inside the pbo, what the row shows
    Graph graph;
    int statementsLowered = 0;
    int statementsTotal = 0;
    // A method is one of three things and only the first two are comparable:
    // it became nodes, it stayed text, or it had no body to model.
    int methodsAsNodes = 0;
    int methodsAsText = 0;
    int methodsEmpty = 0;

    int modelledPercent() const;
};

struct ModOpenResult {
    bool ok = false;
    QString error;
    QString folder;
    QString modName;
    QVector<ModScriptFile> files;
    QVector<ModClassView> classes;
    // One line per file the importer would not model, naming the file and the
    // reason. A mod full of these is worth knowing about before you go hunting
    // for a graph that is not there.
    //
    // Capped. Three of the installed mods are run through an obfuscator that
    // pads the entry table with over a hundred thousand junk names, and one
    // note each would be a list nobody can read and a lot of memory to hold.
    QStringList notes;
    int notesDropped = 0;
    int filesExtracted = 0;
    int filesImported = 0;
    // Entries that could not be produced: a name the reader would not turn into
    // a path, bytes it would not decode, or a file the importer refused.
    int filesRefused = 0;
    int statementsLowered = 0;
    int statementsTotal = 0;
    int methodsAsNodes = 0;
    int methodsAsText = 0;
    int methodsEmpty = 0;
    bool truncated = false;   // a cap in ModOpenOptions stopped the run early

    int modelledPercent() const;
};

// Work in progress, so a caller can spend the import a few files at a time.
// Build one with beginOpen, drive it with openModStep until done() is true,
// then take result().
class ModOpenJob {
public:
    ModOpenJob() = default;

    bool done() const { return m_done; }
    int filesTotal() const { return m_files.size(); }
    int filesDone() const { return m_next; }
    const ModOpenResult &result() const { return m_result; }
    QString modFolder() const { return m_result.folder; }

private:
    friend ModOpenJob beginOpen(const ModEntry &, const ModOpenOptions &);
    friend bool openModStep(ModOpenJob &, const Catalog &, const Builtins &,
                            const Project &, int);

    QVector<ModScriptFile> m_files;
    ModOpenResult m_result;
    int m_next = 0;
    bool m_done = false;
};

// Extracts every .c the mod's pbos carry into the script cache and returns a
// job standing at the first of them. Extraction is the part that touches no
// Catalog, so it is done here in one go; the importing is what gets spread out.
// A mod with no readable pbo comes back done, with ok = false and the reason.
//
// A mod carrying looseScripts is read where those files already are. Nothing is
// copied for them, which keeps the same promise the pbo path keeps by
// extracting into the app's own cache: opening somebody's mod does not write
// anything into their folder.
ModOpenJob beginOpen(const ModEntry &mod, const ModOpenOptions &opts = {});

// Imports up to `budget` more files. Returns true while there is more to do.
bool openModStep(ModOpenJob &job, const Catalog &cat, const Builtins &builtins,
                 const Project &project, int budget = 8);

// Extract and import in one call. Same result as driving the job to the end,
// which is what the test and any headless caller wants.
ModOpenResult openMod(const ModEntry &mod, const Catalog &cat, const Builtins &builtins,
                      const Project &project, const ModOpenOptions &opts = {});

// ------------------------------------------------------------- read only mark

// A graph produced by opening somebody else's mod. The flag rides in
// Graph::extra, which the .sdzn reader and writer carry through untouched, so
// it survives a save into the user's own project and still says where the code
// came from.
void markGraphReadOnly(Graph &g, const QString &modName, const QString &pbo,
                       const QString &entry);
bool graphIsReadOnly(const Graph &g);
// "3D Printer: Project3DPrinter_Scripts.pbo/scripts/4_World/printer.c", or empty.
QString graphOrigin(const Graph &g);

// ------------------------------------------------------- what may be written
//
// A browsed graph is a reader, not a source file. It is allowed to sit in the
// project and to be saved into the .sdzn, because coming back to what you read
// is the point of the browser, and the mark above rides the file so it comes
// back read only. What it may never do is be generated into a mod folder.
// Export scripts leaves it out, Save script to file turns it down, and the .c
// writer under both drops it, so no write the app makes on its own puts another
// author's class in the user's mod.
//
// The rule is here rather than in the window so that every write site asks one
// question, and so a test can ask it without building a window.
bool scriptIsWritable(const ScriptEntry &script);

// One key per file on disk, for comparing two paths that name the same script.
// Cleaned rather than canonical: a file about to be written has no canonical
// path yet. Case folded on Windows, where PlayerInfo.c and playerinfo.c are one
// file. The plan below and whoever writes it have to agree on this, or a file
// with two classes in it is written once per class and the second write wins.
QString scriptFileKey(const QString &path);
bool sameScriptFile(const QString &a, const QString &b);

// One file an export would write. The pointers are into the project that was
// planned, so a plan is only good for as long as that project holds still.
struct ExportTarget {
    // The first script bound for `path`. A file holding two classes appears
    // once, and writing it regenerates the whole file rather than this class.
    const ScriptEntry *script = nullptr;
    QString path;             // absolute
    bool intoSource = false;  // it came from a .c and goes back to that .c
};

// Writable scripts that never came from a file, so an export has to be told
// where to put them. Zero means Export scripts has nothing to ask about.
int scriptsNeedingFolder(const Project &project);

// What Export scripts would write, in project order, one entry per file.
// `dir` is the folder chosen for scripts with no file behind them and may be
// empty when scriptsNeedingFolder says there are none. Anything the mod browser
// produced is left out, and named in `browsed` so the caller can say so.
QVector<ExportTarget> exportPlan(const Project &project, const QString &dir,
                                 QVector<const ScriptEntry *> *browsed = nullptr);

// -------------------------------------------------------------- the library

class ModLibrary : public QObject {
    Q_OBJECT
public:
    explicit ModLibrary(QObject *parent = nullptr);
    ~ModLibrary() override;

    // The Steam workshop folder and the work drive's Mods folder, whichever of
    // the two exist on this machine. Neither existing is a normal state: the
    // user then adds a folder by hand.
    static QStringList defaultRoots();

    QStringList roots() const { return m_roots; }
    void setRoots(const QStringList &roots);
    // False when the folder is not a directory or is already a root.
    bool addRoot(const QString &folder);
    bool removeRoot(const QString &folder);

    // The scan's mods, with anything opened by path in front of them. One list,
    // because a row is a row whichever way it got here.
    QVector<ModEntry> mods() const;
    const ModEntry *mod(const QString &folder) const;

    // A mod opened by path rather than found by the scan. Held apart from the
    // scan so a rescan cannot drop the row the user is looking at, and written
    // to the cache so it is still listed next launch. Returns the entry's
    // folder, which is the key every other call here takes, or empty when the
    // entry is not valid. Re-adding a path replaces what was there.
    QString addOpened(const ModEntry &entry);
    bool removeOpened(const QString &folder);
    QVector<ModEntry> openedMods() const { return m_opened; }

    // Starts a scan on the worker thread and returns at once. Entries whose
    // folder and modified time match the cache are reused rather than re-read,
    // so the second run over 254 mods costs a directory walk. `force` throws
    // that away and reads every pbo header again.
    void refresh(bool force = false);
    bool isScanning() const;
    void cancelScan();

    // Blocking scan. The worker calls this, and so does anything headless.
    // `known` is the previous result keyed by folder; `cancelled` is polled
    // between mods so a long scan can be dropped.
    static QVector<ModEntry> scanRoots(const QStringList &roots,
                                       const QHash<QString, ModEntry> &known = {},
                                       const std::function<bool()> &cancelled = {});
    // One folder, read from disk with no cache in the way.
    static ModEntry readMod(const QString &folder);

    // Where the scan is remembered between runs.
    QString cacheFile() const;
    bool loadCache(QString *error = nullptr);
    bool saveCache(QString *error = nullptr) const;

    // Where extracted scripts go. Never inside a mod folder, which is the whole
    // point: opening a mod to read it must not touch the folder it came from.
    static QString scriptCacheRoot();
    static bool isInsideScriptCache(const QString &path);
    static void clearScriptCache();

signals:
    void scanStarted();
    void scanProgress(int done, int total);
    // The mod list changed, whether from a cache load or a finished scan.
    void modsChanged();
    void scanFinished(int found, bool cancelled);

private slots:
    void onScanFinished();

private:
    QStringList m_roots;
    QVector<ModEntry> m_mods;
    QVector<ModEntry> m_opened;
    // A ScanThread, which is declared in the .cpp because nothing outside it
    // needs the type. Null whenever no scan is in flight.
    QThread *m_scan = nullptr;
};
