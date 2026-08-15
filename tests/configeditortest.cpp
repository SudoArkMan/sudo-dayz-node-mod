// The config editor's rules, and what a save actually writes.
//
// Two things are worth proving here and neither can be judged by looking at the
// window. First, that every rule fires on a config that has the problem and
// stays quiet on one that does not: a checker that cries wolf gets turned off,
// and one that misses a bad path costs an evening. Second, that editing one
// value rewrites one line. The model guarantees the round trip; this is the
// check that the editor is going through it rather than round the side.
//
// The offscreen platform is forced here rather than left to the caller, since a
// test that only passes with a display is a test nobody runs.
//   ./tests/configeditortest ../resources [--shot config.png]
#include "config/configtree.h"
#include "project.h"
#include "theme.h"
#include "widgets/codeeditor.h"
#include "widgets/configeditor.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QPushButton>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>

#include <functional>

static int failures = 0;

static void check(bool ok, const QString &what)
{
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) failures++;
}

static void equal(const QString &got, const QString &want, const QString &what)
{
    const bool ok = got == want;
    QTextStream out(stdout);
    out << (ok ? "  ok   " : "  FAIL ") << what;
    if (!ok) out << " (wanted [" << want << "], got [" << got << "])";
    out << Qt::endl;
    if (!ok) failures++;
}

static QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QString text = QString::fromUtf8(file.readAll());
    text.replace(QLatin1String("\r\n"), QLatin1String("\n"));
    return text;
}

static bool writeAll(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write(text.toUtf8());
    return true;
}

// How many lines differ between two versions of the same file. This is the
// number the whole "do not rewrite the file" rule comes down to.
static int changedLines(const QString &before, const QString &after)
{
    const QStringList a = before.split(QLatin1Char('\n'));
    const QStringList b = after.split(QLatin1Char('\n'));
    int changed = qAbs(a.size() - b.size());
    for (int i = 0; i < qMin(a.size(), b.size()); ++i)
        if (a.at(i) != b.at(i)) changed++;
    return changed;
}

// Both directions of the sync are on timers, since neither reparsing on every
// keystroke nor re-rendering the file on every keystroke is something a typist
// should have to sit through. So the test waits the way a user does.
static void settle(const std::function<bool()> &done, int milliseconds = 3000)
{
    QElapsedTimer clock;
    clock.start();
    while (!done() && clock.elapsed() < milliseconds) {
        QApplication::processEvents(QEventLoop::AllEvents, 20);
        QThread::msleep(5);
    }
    QApplication::processEvents();
}

static int countLevel(const QVector<ConfigFinding> &findings, ConfigFinding::Level level)
{
    int n = 0;
    for (const ConfigFinding &f : findings)
        if (f.level == level) n++;
    return n;
}

static bool mentions(const QVector<ConfigFinding> &findings, const QString &needle)
{
    for (const ConfigFinding &f : findings)
        if (f.text.contains(needle)) return true;
    return false;
}

// The user's own mod, laid down the way it is on their disk: the template with
// its token replaced, name and author filled in by the scaffolder, and the four
// module folders present. Everything about it is right except the patch class,
// which the template hardcodes and its own Init.ps1 never renames.
static QString sudoTest3Config(const QString &templateConfig)
{
    QString text = readAll(templateConfig);
    text.replace(QLatin1String("ModTemplate"), QLatin1String("SudoTest3"));
    text.replace(QLatin1String("name=\"\""), QLatin1String("name=\"Sudo Test 3\""));
    text.replace(QLatin1String("author=\"\""), QLatin1String("author=\"Dillan\""));
    return text;
}

int main(int argc, char *argv[])
{
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    QTextStream out(stdout);

    const QString root = argc > 1 && !QString::fromLocal8Bit(argv[1]).startsWith(QLatin1Char('-'))
                             ? QString::fromLocal8Bit(argv[1])
                             : QStringLiteral("resources");

    const QString templateConfig =
        QDir(root).filePath(QStringLiteral("mod-template/ModTemplate/Scripts/config.cpp"));
    check(QFileInfo(templateConfig).isFile(),
          QStringLiteral("the bundled template config is where it should be"));
    if (!QFileInfo(templateConfig).isFile()) return 1;

    // ---- the mod folder the user actually has -----------------------------
    out << "\nA real mod folder, everything right but the patch class" << Qt::endl;

    QTemporaryDir temp;
    check(temp.isValid(), QStringLiteral("temporary mod folder"));
    if (!temp.isValid()) return 1;

    const QString modRoot = temp.filePath(QStringLiteral("SudoTest3Mod"));
    const QString scripts = modRoot + QStringLiteral("/SudoTest3/Scripts");
    for (const char *module : { "1_Core", "3_Game", "4_World", "5_Mission" })
        QDir().mkpath(scripts + QLatin1Char('/') + QString::fromLatin1(module));
    writeAll(scripts + QStringLiteral("/Inputs.xml"), QStringLiteral("<inputs/>\n"));

    const QString realConfig = scripts + QStringLiteral("/config.cpp");
    const QString realText = sudoTest3Config(templateConfig);
    check(realText.contains(QLatin1String("MT_Scripts")),
          QStringLiteral("the template leaves MT_Scripts in a mod called something else"));
    writeAll(realConfig, realText);

    Project noProject;
    const ConfigContext context = configContextFor(realConfig, noProject);
    equal(context.prefix, QStringLiteral("SudoTest3"),
          QStringLiteral("the prefix is read off the config's own folder"));
    check(context.modRoot == QDir::cleanPath(modRoot),
          QStringLiteral("the mod root is the folder above the prefix"));
    check(context.onDisk(), QStringLiteral("the context points at a real mod folder"));

    ConfigFile file = parseConfig(realText);
    check(file.errors.isEmpty(), QStringLiteral("the config parses with nothing left over"));

    QVector<ConfigFinding> findings = validateConfig(file, context);
    equal(QString::number(findings.size()), QStringLiteral("1"),
          QStringLiteral("one finding on a mod whose only problem is the patch class"));
    if (!findings.isEmpty()) {
        const ConfigFinding &first = findings.first();
        check(first.level == ConfigFinding::Level::Error,
              QStringLiteral("the patch class collision is an error"));
        equal(first.path, QStringLiteral("CfgPatches/MT_Scripts"),
              QStringLiteral("the finding points at the class it is about"));
        check(first.fix == ConfigFinding::Fix::RenamePatchClass,
              QStringLiteral("it comes with a fix"));
        equal(first.fixValue, QStringLiteral("SudoTest3_Scripts"),
              QStringLiteral("the fix is the name the mod should have had"));
        check(first.text.contains(QLatin1String("MT_Scripts")),
              QStringLiteral("the finding names the class"));
    }

    // ---- the fix, and what it costs the file ------------------------------
    out << "\nThe fix rewrites one line and leaves the rest alone" << Qt::endl;

    equal(writeConfig(file), realText,
          QStringLiteral("writing back an untouched config gives the same bytes"));

    if (ConfigClass *patch = findClass(file, QStringLiteral("CfgPatches/MT_Scripts"))) {
        patch->name = QStringLiteral("SudoTest3_Scripts");
        const QString after = writeConfig(file);
        equal(QString::number(changedLines(realText, after)), QStringLiteral("1"),
              QStringLiteral("renaming the patch class changes one line"));
        check(after.contains(QLatin1String("class SudoTest3_Scripts")),
              QStringLiteral("the new name is in the file"));
        check(!after.contains(QLatin1String("MT_Scripts")),
              QStringLiteral("the old name is gone"));

        const ConfigFile again = parseConfig(after);
        check(validateConfig(again, context).isEmpty(),
              QStringLiteral("the fixed config has nothing left to flag"));
    } else {
        check(false, QStringLiteral("the patch class can be found by path"));
    }

    // A value edit is the other half of the same promise: the panel writes one
    // property and the file keeps its shape.
    {
        ConfigFile edited = parseConfig(realText);
        ConfigClass *mod = findClass(edited, QStringLiteral("CfgMods/SudoTest3"));
        ConfigValue *name = mod ? findValue(*mod, QStringLiteral("name")) : nullptr;
        check(name != nullptr, QStringLiteral("the launcher name is reachable by path"));
        if (name) {
            name->scalar = QStringLiteral("\"Sudo Test Three\"");
            const QString after = writeConfig(edited);
            equal(QString::number(changedLines(realText, after)), QStringLiteral("1"),
                  QStringLiteral("editing one property changes one line"));
        }
    }

    // ---- each rule, one at a time -----------------------------------------
    out << "\nOne broken thing at a time" << Qt::endl;

    // 2. A module path that is not on disk.
    {
        QString broken = realText;
        broken.replace(QLatin1String("SudoTest3/Scripts/3_Game"),
                       QLatin1String("SudoTest3/Scripts/3_Games"));
        const QVector<ConfigFinding> found = validateConfig(parseConfig(broken), context);
        check(mentions(found, QStringLiteral("3_Games")),
              QStringLiteral("a mistyped module path is reported"));
        check(mentions(found, QStringLiteral("gameScriptModule")),
              QStringLiteral("and it names the module it is under"));
        equal(QString::number(countLevel(found, ConfigFinding::Level::Error)),
              QStringLiteral("2"),
              QStringLiteral("the mistyped path joins the patch class, and nothing else"));
    }

    // 3. dir naming a folder that is not there.
    {
        QString broken = realText;
        broken.replace(QLatin1String("dir=\"SudoTest3\""), QLatin1String("dir=\"SudoTest4\""));
        const QVector<ConfigFinding> found = validateConfig(parseConfig(broken), context);
        check(mentions(found, QStringLiteral("SudoTest4")),
              QStringLiteral("a dir that does not match the mod folder is reported"));
    }

    // 5. A class name that does not match its own dir, with dir itself right.
    {
        QString broken = realText;
        broken.replace(QLatin1String("class SudoTest3\n    {\n        name="),
                       QLatin1String("class SudoTest3Mod\n    {\n        name="));
        const ConfigFile parsed = parseConfig(broken);
        const bool renamed = findClass(parsed, QStringLiteral("CfgMods/SudoTest3Mod")) != nullptr;
        check(renamed, QStringLiteral("the CfgMods entry can be renamed for the test"));
        if (renamed) {
            const QVector<ConfigFinding> found = validateConfig(parsed, context);
            check(mentions(found, QStringLiteral("SudoTest3Mod")),
                  QStringLiteral("a class that disagrees with its dir is reported"));
        }
    }

    // 4. inputs pointing at a file nobody shipped.
    {
        QString broken = realText;
        broken.replace(QLatin1String("SudoTest3/Scripts/Inputs.xml"),
                       QLatin1String("SudoTest3/Scripts/Input.xml"));
        const QVector<ConfigFinding> found = validateConfig(parseConfig(broken), context);
        check(mentions(found, QStringLiteral("Input.xml")),
              QStringLiteral("an inputs file that is not there is reported"));
        check(countLevel(found, ConfigFinding::Level::Warning) == 1,
              QStringLiteral("and it is a warning, not a load failure"));
    }

    // 6. The two fields the launcher shows.
    {
        QString broken = realText;
        broken.replace(QLatin1String("author=\"Dillan\""), QLatin1String("author=\"\""));
        const QVector<ConfigFinding> found = validateConfig(parseConfig(broken), context);
        check(mentions(found, QStringLiteral("author")),
              QStringLiteral("an empty author is reported"));
    }

    // A config opened from somewhere that is not a mod folder gets the rules
    // that read the file and none of the rules that read the disk, or every path
    // in it would come back as missing.
    {
        const QString loose = temp.filePath(QStringLiteral("loose/config.cpp"));
        writeAll(loose, realText);
        const ConfigContext nowhere = configContextFor(loose, noProject);
        check(!nowhere.onDisk(), QStringLiteral("a config outside a mod folder knows it"));
        const QVector<ConfigFinding> found = validateConfig(parseConfig(realText), nowhere);
        check(!mentions(found, QStringLiteral("3_Game")),
              QStringLiteral("and no path is called missing on the strength of that"));
    }

    // ---- the template itself ----------------------------------------------
    out << "\nThe bundled template, which is where MT_Scripts comes from" << Qt::endl;

    const QString templateText = readAll(templateConfig);
    const ConfigFile templateFile = parseConfig(templateText);
    const ConfigContext templateContext = configContextFor(templateConfig, noProject);
    equal(templateContext.prefix, QStringLiteral("ModTemplate"),
          QStringLiteral("the template's prefix comes off its own folder"));
    const QVector<ConfigFinding> templateFindings =
        validateConfig(templateFile, templateContext);
    check(mentions(templateFindings, QStringLiteral("MT_Scripts")),
          QStringLiteral("the template's own config is flagged for the patch class"));
    check(mentions(templateFindings, QStringLiteral("no name")),
          QStringLiteral("and for the empty launcher name it ships with"));

    // ---- the window, driven from the outside ------------------------------
    out << "\nThe window itself" << Qt::endl;
    {
        auto *editor = new ConfigEditor(nullptr, nullptr, realConfig);
        QString error;
        if (!editor->load(&error)) {
            check(false, QStringLiteral("the editor opens the mod's config (%1)").arg(error));
        } else {
            equal(QString::number(editor->findings().size()), QStringLiteral("1"),
                  QStringLiteral("the window finds what the rules found"));

            QPushButton *fix = nullptr;
            for (QPushButton *button : editor->findChildren<QPushButton *>())
                if (button->text().startsWith(QLatin1String("Rename"))) fix = button;
            check(fix != nullptr, QStringLiteral("the finding carries a button"));

            if (fix) {
                fix->click();
                // The fix is applied on the next turn of the event loop, since
                // the button it came from is about to be deleted.
                settle([editor]() { return editor->findings().isEmpty(); });
                check(editor->findings().isEmpty(),
                      QStringLiteral("one click clears the finding"));

                CodeEditor *pane = editor->findChild<CodeEditor *>();
                check(pane != nullptr, QStringLiteral("the file text is under the tree"));
                if (pane) {
                    check(pane->toPlainText().contains(
                              QLatin1String("class SudoTest3_Scripts")),
                          QStringLiteral("and the text under it has the new name"));
                    check(!pane->toPlainText().contains(QLatin1String("MT_Scripts")),
                          QStringLiteral("with the old one gone"));
                }
            }

            // The other direction: text typed into the pane reaches the rules.
            if (CodeEditor *pane = editor->findChild<CodeEditor *>()) {
                QString typed = pane->toPlainText();
                typed.replace(QLatin1String("SudoTest3/Scripts/4_World"),
                              QLatin1String("SudoTest3/Scripts/4_Worlds"));
                pane->setPlainText(typed);
                settle([editor]() { return !editor->findings().isEmpty(); });
                check(mentions(editor->findings(), QStringLiteral("4_Worlds")),
                      QStringLiteral("a path typed into the text is checked as well"));
            }
        }
        delete editor;
    }

    // ---- the window -------------------------------------------------------
    const QStringList args = QCoreApplication::arguments();
    const auto argAfter = [&args](const QString &flag) {
        const int at = args.indexOf(flag);
        return at > 0 && at + 1 < args.size() ? args.at(at + 1) : QString();
    };
    const QString shot = argAfter(QStringLiteral("--shot"));
    const QString moduleShot = argAfter(QStringLiteral("--shot-module"));
    if (!shot.isEmpty() || !moduleShot.isEmpty()) {
        theme::apply(app);
        auto *editor = new ConfigEditor(nullptr, nullptr, templateConfig);
        QString error;
        if (!editor->load(&error)) {
            check(false, QStringLiteral("the editor opens the template config (%1)").arg(error));
        } else {
            check(editor->findings().size() == templateFindings.size(),
                  QStringLiteral("the window shows the same findings the rules found"));
            editor->resize(1040, 780);
            editor->show();
            QApplication::processEvents();
            if (!shot.isEmpty())
                out << (editor->grab().save(shot) ? "wrote " : "could not write ") << shot
                    << Qt::endl;
            if (!moduleShot.isEmpty()) {
                // The module list is the part worth looking at twice: this is
                // where a path that is not on disk has to be visible while it is
                // being typed, not only in the findings.
                editor->selectPath(
                    QStringLiteral("CfgMods/ModTemplate/defs/gameScriptModule"));
                QApplication::processEvents();
                out << (editor->grab().save(moduleShot) ? "wrote " : "could not write ")
                    << moduleShot << Qt::endl;
            }
        }
        delete editor;
    }

    out << (failures == 0 ? "\nall good" : QStringLiteral("\n%1 failed").arg(failures))
        << Qt::endl;
    return failures == 0 ? 0 : 1;
}
