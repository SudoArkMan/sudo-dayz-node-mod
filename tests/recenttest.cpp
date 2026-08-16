// The recent projects list, and the page built on it.
//
// Every project file here is written by the test into a QTemporaryDir, and the
// store is a file of its own in the same place, so a run never reads or writes
// the user's real history.
//
// The list has to survive the four things that actually happen to it: the same
// project opened twice, the same project reached by two spellings of its path,
// more projects than it can hold, and a project whose folder has been moved.
// The last one is the interesting case: the entry has to stay and say so,
// because an entry that vanishes leaves the user nothing to search for.
//
// The offscreen platform is forced here rather than left to the caller's
// environment: a test that only passes with a display is a test nobody runs.
//   ./tests/recenttest ../resources [--shot start.png] [--gallery templates.png]
#include "recentprojects.h"
#include "theme.h"
#include "widgets/newscriptdialog.h"
#include "widgets/startpage.h"

#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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

// A .sdzn with `scripts` scripts in it, `nodes` nodes in each. Small, but the
// same shape the real loader reads, so the summary reader is being pointed at a
// real project file rather than at a fixture invented for it.
static bool writeProject(const QString &path, const QString &name,
                         const QString &modPrefix, int scripts, int nodes)
{
    QJsonArray scriptArray;
    for (int s = 0; s < scripts; ++s) {
        QJsonArray nodeArray;
        for (int n = 0; n < nodes; ++n) {
            QJsonObject node;
            node.insert("id", QStringLiteral("n%1").arg(n + 1));
            node.insert("kind", "call");
            node.insert("ref", "m1");
            node.insert("x", 40 * n);
            node.insert("y", 60);
            nodeArray.append(node);
        }
        QJsonObject graph;
        graph.insert("className", QStringLiteral("%1_%2").arg(name).arg(s + 1));
        graph.insert("module", "4_World");
        graph.insert("nodes", nodeArray);
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
    if (!modPrefix.isEmpty()) root.insert("modPrefix", modPrefix);

    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    return file.error() == QFileDevice::NoError;
}

int main(int argc, char *argv[])
{
    // Offscreen unless the caller has already chosen, so a shot can be taken on
    // a platform that has fonts.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("SUDO DayZ Node Mod"));
    QCoreApplication::setApplicationVersion(QStringLiteral("0.1.0"));
    QTextStream stream(stdout);
    out = &stream;

    QTemporaryDir temp;
    if (!temp.isValid()) {
        line(QStringLiteral("could not make a temporary folder"));
        return 1;
    }
    const QDir root(temp.path());
    const QString store = root.filePath(QStringLiteral("store/recent.json"));

    // ---- reading a project without opening it -----------------------------
    line(QStringLiteral("summary"));
    {
        const QString path = root.filePath(QStringLiteral("Alpha/Alpha.sdzn"));
        check(writeProject(path, QStringLiteral("Alpha"), QStringLiteral("SUDO_Alpha"),
                           3, 4),
              QStringLiteral("a project file is written"));

        RecentProject summary;
        QString error;
        check(readProjectSummary(path, summary, &error),
              QStringLiteral("the summary reads it (%1)").arg(error));
        equal(summary.name, QStringLiteral("Alpha"), QStringLiteral("name"));
        equal(summary.modName, QStringLiteral("SUDO_Alpha"), QStringLiteral("mod name"));
        equal(summary.scriptCount, 3, QStringLiteral("script count"));
        equal(summary.nodeCount, 12, QStringLiteral("node count over every script"));
        check(summary.countsKnown(),
              QStringLiteral("the counts are stamped with the file's timestamp"));

        RecentProject broken;
        QFile junk(root.filePath(QStringLiteral("broken.sdzn")));
        check(junk.open(QIODevice::WriteOnly), QStringLiteral("a damaged file is written"));
        junk.write("{ this is not json");
        junk.close();
        check(!readProjectSummary(junk.fileName(), broken),
              QStringLiteral("a damaged file is refused rather than half read"));
    }

    // ---- add, reorder, dedupe ---------------------------------------------
    line(QStringLiteral("\nthe list"));
    {
        RecentProjects recent(store);
        recent.load();
        equal(recent.size(), 0, QStringLiteral("a first run starts empty"));

        const QString alpha = root.filePath(QStringLiteral("Alpha/Alpha.sdzn"));
        const QString beta = root.filePath(QStringLiteral("Beta/Beta.sdzn"));
        writeProject(beta, QStringLiteral("Beta"), QString(), 1, 2);

        check(recent.record(alpha), QStringLiteral("recording a project reads it"));
        equal(recent.size(), 1, QStringLiteral("one entry"));
        equal(recent.entries().first().name, QStringLiteral("Alpha"),
              QStringLiteral("the entry is the project that was opened"));
        equal(recent.entries().first().scriptCount, 3,
              QStringLiteral("its counts came off the file"));

        recent.record(beta);
        equal(recent.size(), 2, QStringLiteral("a second project is a second entry"));
        equal(recent.entries().first().name, QStringLiteral("Beta"),
              QStringLiteral("the newest is at the front"));

        recent.record(alpha);
        equal(recent.size(), 2,
              QStringLiteral("reopening one already listed adds no second row"));
        equal(recent.entries().first().name, QStringLiteral("Alpha"),
              QStringLiteral("reopening moves it back to the front"));

        // The same file, spelled the way a drag from a folder or a relative
        // path would spell it. Two rows for one project is the bug this stops.
        const QString roundabout =
            root.filePath(QStringLiteral("Alpha/../Alpha/./Alpha.sdzn"));
        recent.record(roundabout);
        equal(recent.size(), 2,
              QStringLiteral("a path spelled differently is the same project"));
        check(recent.contains(alpha),
              QStringLiteral("and it is still found under the plain spelling"));
#ifdef Q_OS_WIN
        recent.record(alpha.toUpper());
        equal(recent.size(), 2,
              QStringLiteral("case does not make a second entry on Windows"));
#endif
        check(!recent.lastFolder().isEmpty(),
              QStringLiteral("the folder to open next is remembered"));
    }

    // ---- the cap -----------------------------------------------------------
    line(QStringLiteral("\nthe cap"));
    {
        const QString capStore = root.filePath(QStringLiteral("store/cap.json"));
        RecentProjects recent(capStore);
        recent.load();
        recent.setCapacity(5);
        for (int i = 0; i < 9; ++i) {
            const QString path =
                root.filePath(QStringLiteral("Cap/P%1.sdzn").arg(i));
            writeProject(path, QStringLiteral("P%1").arg(i), QString(), 1, 1);
            recent.record(path);
        }
        equal(recent.size(), 5, QStringLiteral("the list stops at its capacity"));
        equal(recent.entries().first().name, QStringLiteral("P8"),
              QStringLiteral("the newest survives"));
        equal(recent.entries().last().name, QStringLiteral("P4"),
              QStringLiteral("the oldest inside the cap survives"));
        check(!recent.contains(root.filePath(QStringLiteral("Cap/P0.sdzn"))),
              QStringLiteral("what fell off the end is gone"));
    }

    // ---- the round trip ----------------------------------------------------
    line(QStringLiteral("\nthe round trip"));
    {
        RecentProjects written(store);
        written.load();
        const QVector<RecentProject> before = written.entries();

        RecentProjects read(store);
        QString error;
        check(read.load(&error), QStringLiteral("the store loads (%1)").arg(error));
        equal(read.size(), before.size(), QStringLiteral("same number of entries"));
        bool sameOrder = read.size() == before.size();
        bool sameCounts = sameOrder;
        for (int i = 0; sameOrder && i < before.size(); ++i) {
            sameOrder = recentKey(read.entries().at(i).path)
                        == recentKey(before.at(i).path);
            sameCounts = sameCounts
                         && read.entries().at(i).scriptCount == before.at(i).scriptCount
                         && read.entries().at(i).nodeCount == before.at(i).nodeCount
                         && read.entries().at(i).modName == before.at(i).modName;
        }
        check(sameOrder, QStringLiteral("in the same order"));
        check(sameCounts, QStringLiteral("with their counts and mod names"));
        equal(read.lastFolder(), written.lastFolder(),
              QStringLiteral("and the folder to open next"));
        check(read.entries().first().lastOpened.isValid()
                  && qAbs(read.entries().first().lastOpened.secsTo(
                         before.first().lastOpened)) <= 1,
              QStringLiteral("times come back as they went in"));
    }

    // ---- counts are cached against the file timestamp ----------------------
    line(QStringLiteral("\nthe counts cache"));
    {
        const QString cacheStore = root.filePath(QStringLiteral("store/cache.json"));
        const QString path = root.filePath(QStringLiteral("Cache/Cache.sdzn"));
        writeProject(path, QStringLiteral("Cache"), QString(), 2, 3);
        RecentProjects recent(cacheStore);
        recent.load();
        recent.record(path);
        equal(recent.entry(path).nodeCount, 6, QStringLiteral("counted once"));

        // Rewritten with more in it, then given its old timestamp back. A pass
        // that re-reads every file would report the new numbers; the point of
        // the cache is that this one does not.
        const QDateTime stamp = QFileInfo(path).lastModified();
        writeProject(path, QStringLiteral("Cache"), QString(), 4, 3);
        QFile file(path);
        bool restored = file.open(QIODevice::ReadWrite);
        restored = restored
                   && file.setFileTime(stamp, QFileDevice::FileModificationTime);
        file.close();
        recent.refresh();
        if (restored) {
            equal(recent.entry(path).nodeCount, 6,
                  QStringLiteral("a file whose timestamp has not moved is not read again"));
        } else {
            line(QStringLiteral("  skip the timestamp could not be set back"));
        }

        // Now let the timestamp move, which is what an edit outside the app
        // really looks like.
        writeProject(path, QStringLiteral("Cache"), QString(), 4, 3);
        recent.refresh();
        equal(recent.entry(path).nodeCount, 12,
              QStringLiteral("a file that has changed is read again"));
        equal(recent.entry(path).scriptCount, 4, QStringLiteral("with its new counts"));
    }

    // ---- a project whose file has gone -------------------------------------
    line(QStringLiteral("\na project that moved"));
    {
        const QString goneStore = root.filePath(QStringLiteral("store/gone.json"));
        const QString path = root.filePath(QStringLiteral("Gone/Gone.sdzn"));
        writeProject(path, QStringLiteral("Gone"), QStringLiteral("SUDO_Gone"), 2, 5);
        RecentProjects recent(goneStore);
        recent.load();
        recent.record(path);
        writeProject(root.filePath(QStringLiteral("Stays/Stays.sdzn")),
                     QStringLiteral("Stays"), QString(), 1, 1);
        recent.record(root.filePath(QStringLiteral("Stays/Stays.sdzn")));

        check(QFile::remove(path), QStringLiteral("the project file is deleted"));
        equal(recent.refresh(), 1, QStringLiteral("the refresh counts one missing"));
        equal(recent.size(), 2,
              QStringLiteral("the entry stays in the list rather than vanishing"));
        check(recent.entry(path).missing,
              QStringLiteral("and is marked as not being there"));
        equal(recent.entry(path).name, QStringLiteral("Gone"),
              QStringLiteral("it still knows what it was"));
        equal(recent.entry(path).scriptCount, 2,
              QStringLiteral("and keeps the counts it had"));

        // Recording a missing project is refused, but the row still moves: the
        // user just tried to open it, which is when they most want to find it.
        check(!recent.record(path), QStringLiteral("recording it again reports failure"));
        equal(recent.size(), 2, QStringLiteral("without adding a duplicate"));

        RecentProjects reread(goneStore);
        reread.load();
        equal(reread.size(), 2, QStringLiteral("a missing entry survives the store"));

        equal(recent.removeMissing(), 1, QStringLiteral("removing the missing takes one"));
        equal(recent.size(), 1, QStringLiteral("leaving the project that is still there"));
        check(!recent.contains(path), QStringLiteral("and the gone one is out"));
    }

    // ---- relative time -----------------------------------------------------
    line(QStringLiteral("\nrelative time"));
    {
        const QDateTime now = QDateTime(QDate(2026, 8, 16), QTime(14, 0));
        equal(relativeTime(now.addSecs(-20), now), QStringLiteral("just now"),
              QStringLiteral("seconds"));
        equal(relativeTime(now.addSecs(-60), now), QStringLiteral("1 minute ago"),
              QStringLiteral("one minute is not plural"));
        equal(relativeTime(now.addSecs(-3600 * 5), now), QStringLiteral("5 hours ago"),
              QStringLiteral("hours"));
        equal(relativeTime(now.addDays(-1), now), QStringLiteral("yesterday"),
              QStringLiteral("yesterday, by the calendar"));
        equal(relativeTime(now.addDays(-4), now), QStringLiteral("4 days ago"),
              QStringLiteral("days"));
        equal(relativeTime(now.addDays(-9), now), QStringLiteral("1 week ago"),
              QStringLiteral("weeks"));
        equal(relativeTime(now.addDays(-400), now), QStringLiteral("12 Jul 2025"),
              QStringLiteral("past a month it is a date"));
        equal(relativeTime(QDateTime(), now), QStringLiteral("never"),
              QStringLiteral("no time at all"));
    }

    // ---- the templates the gallery ships -----------------------------------
    line(QStringLiteral("\ntemplates"));
    {
        const QString resources = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QString();
        const QVector<StartTemplate> templates = startTemplates(resources);
        check(templates.size() >= 4, QStringLiteral("four tiles at least"));

        int scripts = 0;
        int projects = 0;
        bool described = true;
        for (const StartTemplate &tpl : templates) {
            described = described && !tpl.summary.isEmpty() && !tpl.title.isEmpty();
            if (tpl.kind == StartTemplateKind::Project) {
                ++projects;
                continue;
            }
            ++scripts;
            // The skeleton comes from the New script dialog, so a tile can only
            // ship what the app already writes.
            const QString text = scriptSkeleton(tpl.script, nullptr);
            check(!text.trimmed().isEmpty(),
                  QStringLiteral("%1 writes something").arg(tpl.title));
            if (tpl.script.kind == ScriptKind::ModdedClass) {
                check(text.contains(QStringLiteral("modded class ")
                                    + tpl.script.baseClass),
                      QStringLiteral("%1 reopens %2")
                          .arg(tpl.title, tpl.script.baseClass));
                check(text.contains(QStringLiteral("super.")),
                      QStringLiteral("%1 keeps its super call").arg(tpl.title));
            } else {
                check(text.contains(QStringLiteral("class ") + tpl.script.className),
                      QStringLiteral("%1 declares its class").arg(tpl.title));
            }
        }
        check(described, QStringLiteral("every tile says what it is for"));
        check(scripts >= 3, QStringLiteral("three script skeletons"));
        check(projects >= 1, QStringLiteral("and the showcase project"));

        for (const StartTemplate &tpl : templates) {
            if (tpl.kind != StartTemplateKind::Project) continue;
            if (tpl.available)
                check(QFileInfo(tpl.projectPath).isFile(),
                      QStringLiteral("%1 points at a file that is there").arg(tpl.title));
            else
                line(QStringLiteral("  note %1 is not installed beside this build")
                         .arg(tpl.title));
        }
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

        // A store written by hand, so the rows carry a spread of times rather
        // than all reading "just now", and one of them points at a project that
        // is not there.
        const QString pageStore = root.filePath(QStringLiteral("store/page.json"));
        const QDateTime now = QDateTime::currentDateTime();
        const QVector<QStringList> rows = {
            {QStringLiteral("SUDO_Link"), QStringLiteral("SUDO_Link"),
             QStringLiteral("14"), QStringLiteral("212"), QStringLiteral("0")},
            {QStringLiteral("Trader Overhaul"), QStringLiteral("SUDO_Trader"),
             QStringLiteral("6"), QStringLiteral("88"), QStringLiteral("3")},
            {QStringLiteral("Night Ops"), QString(),
             QStringLiteral("2"), QStringLiteral("31"), QStringLiteral("30")},
            {QStringLiteral("Old Cache Test"), QStringLiteral("SUDO_Cache"),
             QStringLiteral("1"), QStringLiteral("9"), QStringLiteral("2600")},
        };
        QJsonArray projects;
        for (int i = 0; i < rows.size(); ++i) {
            const QStringList &row = rows.at(i);
            const QString path =
                root.filePath(QStringLiteral("Page/%1/%1.sdzn").arg(row.at(0)));
            // The last row is left off disk on purpose: the missing state is
            // part of what the page has to draw.
            if (i + 1 < rows.size())
                writeProject(path, row.at(0), row.at(1), row.at(2).toInt(),
                             row.at(3).toInt() / qMax(1, row.at(2).toInt()));
            QJsonObject entry;
            entry.insert("path", QDir::cleanPath(path));
            entry.insert("name", row.at(0));
            if (!row.at(1).isEmpty()) entry.insert("modName", row.at(1));
            entry.insert("scripts", row.at(2).toInt());
            entry.insert("nodes", row.at(3).toInt());
            entry.insert("lastOpened",
                         now.addSecs(-60 * row.at(4).toInt()).toUTC().toString(Qt::ISODate));
            projects.append(entry);
        }
        QJsonObject storeRoot;
        storeRoot.insert("version", 1);
        storeRoot.insert("projects", projects);
        QDir().mkpath(QFileInfo(pageStore).absolutePath());
        QFile storeFile(pageStore);
        check(storeFile.open(QIODevice::WriteOnly),
              QStringLiteral("the page's store is written"));
        storeFile.write(QJsonDocument(storeRoot).toJson(QJsonDocument::Indented));
        storeFile.close();

        RecentProjects recent(pageStore);
        check(recent.load(), QStringLiteral("the page's store loads"));
        equal(recent.size(), rows.size(), QStringLiteral("with a row per project"));

        auto *page = new StartPage(&recent);
        page->resize(1180, 620);
        page->refresh();
        page->show();
        QApplication::processEvents();
        check(page->gallery() != nullptr && page->gallery()->height() > 0,
              QStringLiteral("the templates column has laid itself out"));
        equal(recent.refresh(), 1,
              QStringLiteral("the page's own refresh sees the missing project"));

        const QString shot = argAfter(QStringLiteral("--shot"));
        if (!shot.isEmpty())
            line((page->grab().save(shot) ? QStringLiteral("wrote ")
                                          : QStringLiteral("could not write "))
                 + shot);
        const QString gallery = argAfter(QStringLiteral("--gallery"));
        if (!gallery.isEmpty())
            line((page->gallery()->grab().save(gallery)
                      ? QStringLiteral("wrote ")
                      : QStringLiteral("could not write "))
                 + gallery);
        delete page;
    }

    line(failures == 0 ? QStringLiteral("\nall good")
                       : QStringLiteral("\n%1 failed").arg(failures));
    return failures == 0 ? 0 : 1;
}
