// config.cpp as a class tree: parse it, write it back, and edit it.
//
// The bar is the round trip. writeConfig(parseConfig(x)) has to give back x for
// every config in the repo and for every config.cpp on the work drive when one
// is mounted, because this model is going to open somebody's mod config and a
// lost line is a broken mod.
//
//   ./tests/configtest ../resources [-v]

#include "config/configtree.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QTextStream>

static int fails = 0;
static bool verbose = false;

static void check(bool ok, const QString &what)
{
    QTextStream o(stdout);
    o << (ok ? "  ok   " : "  FAIL ") << what << Qt::endl;
    if (!ok) fails++;
}

// Where the round trip broke, as a line number and the two lines, so a failure
// says what to look at instead of dumping two files.
static QString firstDifference(const QString &a, const QString &b)
{
    int i = 0;
    while (i < a.size() && i < b.size() && a.at(i) == b.at(i)) i++;
    if (i == a.size() && i == b.size()) return QString();
    const int line = a.left(i).count(QLatin1Char('\n')) + 1;
    const auto around = [](const QString &s, int at) {
        const int from = s.lastIndexOf(QLatin1Char('\n'), qMax(0, at - 1)) + 1;
        int to = s.indexOf(QLatin1Char('\n'), at);
        if (to < 0) to = s.size();
        return s.mid(from, to - from).replace(QLatin1Char('\t'), QStringLiteral("\\t"));
    };
    return QStringLiteral("line %1\n           was: %2\n           got: %3")
        .arg(line).arg(around(a, i), around(b, i));
}

static bool roundTrips(const QString &text, QString *why = nullptr)
{
    const ConfigFile f = parseConfig(text);
    const QString out = writeConfig(f);
    if (out == text) return true;
    if (why) *why = firstDifference(text, out);
    return false;
}

// The config the user pasted, byte for byte. CfgPatches still says MT_Scripts
// while everything else says SudoTest3, which is the template leftover two mods
// collide on. Kept exactly as it is on disk: the model has to survive the mixed
// tabs and spaces and the trailing space after `class engineScriptModule`.
static const char *kUserConfig =
    R"CFG(class CfgPatches
{
	class MT_Scripts
	{
		requiredAddons[] = { "DZ_Scripts" };
	};
};

class CfgAddons
{
    class PreloadAddons
    {
        class SudoTest3
        {
            list[]={};
        };
    };
};

class CfgMods
{
    class SudoTest3
    {
        name="SudoTest3";
        dir="SudoTest3";
        picture="";
        action="";
        author="SUDOArkMan";
        overview = "";
		inputs = "SudoTest3/Scripts/Inputs.xml";
		type = "mod";
        defines[] = {};
		dependencies[] =
		{
			"Game", "World", "Mission"
		};

        class defs
		{
			class imageSets
			{
				files[]= {};
			};
			class widgetStyles
			{
				files[]= {};
			};

			class engineScriptModule 
			{ 
				files[] = { "SudoTest3/Scripts/1_Core"};
			};

			class gameScriptModule
			{
				files[] = { "SudoTest3/Scripts/3_Game" };
			};
			class worldScriptModule
			{
				files[] = { "SudoTest3/Scripts/4_World" };
			};

			class missionScriptModule 
			{
				files[] = { "SudoTest3/Scripts/5_Mission" };
			};
		};
    };
};)CFG";

struct Case {
    const char *name;
    const char *text;
};

// One construct each, plus the shapes that break a naive parser: a `//` inside
// a quoted URL, a brace in a string, nested list entries, a trailing comma.
static const Case kCases[] = {
    {"empty file", ""},
    {"comment only", "// nothing here\n"},
    {"one class", "class A\n{\n};\n"},
    {"one line class", "class A { x = 1; };\n"},
    {"scalar string", "class A\n{\n\tname=\"x\";\n};\n"},
    {"scalar number", "class A\n{\n\trequiredVersion=0.1;\n};\n"},
    {"scalar identifier", "class A\n{\n\tvalue = SOME_MACRO;\n};\n"},
    {"negative number", "class A\n{\n\toffset=-1.5;\n};\n"},
    {"empty array", "class A\n{\n\tlist[]={};\n};\n"},
    {"array one line", "class A\n{\n\trequiredAddons[] = { \"DZ_Scripts\" };\n};\n"},
    {"array many lines",
     "class A\n{\n\tdependencies[] =\n\t{\n\t\t\"Game\",\n\t\t\"World\"\n\t};\n};\n"},
    {"array trailing comma", "class A\n{\n\tlist[] = { \"a\", };\n};\n"},
    {"array nested braces", "class A\n{\n\tpairs[] = { {0,1}, {2,3} };\n};\n"},
    {"array append", "class A\n{\n\tlist[] += { \"b\" };\n};\n"},
    {"inheritance", "class A : B\n{\n};\n"},
    {"inheritance tight", "class A: B\n{\n};\n"},
    {"external", "class A;\nclass B : A\n{\n};\n"},
    {"external inside class", "class A\n{\n\tclass B;\n};\n"},
    {"include preamble", "#include \"\\DZ\\data\\basicDefines.hpp\"\n\nclass A\n{\n};\n"},
    {"line comment", "class A\n{\n\t// why\n\tx = 1;\n};\n"},
    {"block comment", "class A\n{\n\t/* why\n\t   because */\n\tx = 1;\n};\n"},
    {"url is not a comment", "class A\n{\n\taction=\"http://dayz.com/\";\n};\n"},
    {"brace in a string", "class A\n{\n\ttexture=\"#(rgb,8,8,3)color(0.5,0.5,1.0,1)\";\n};\n"},
    {"top level scalar", "battleyeLicenceUrl=\"BattlEye\\EULA.txt\";\nclass A\n{\n};\n"},
    {"deep nesting",
     "class A\n{\n\tclass B\n\t{\n\t\tclass C\n\t\t{\n\t\t\tx = 1;\n\t\t};\n\t};\n};\n"},
    {"no trailing newline", "class A\n{\n};"},
    {"crlf", "class A\r\n{\r\n\tx = 1;\r\n};\r\n"},
    {"blank lines kept", "class A\n{\n\n\n\tx = 1;\n\n};\n\n\n"},
    {"negative with no space", "class A\n{\n\tvarTemperatureFreezePoint=-1;\n};\n"},
    {"name starts with a digit", "class 1kHz_mono_1s_SoundShader\n{\n\tx = 1;\n};\n"},
    {"junk statement is kept", "class A\n{\n\tdelete B;\n\tx = 1;\n};\n"},
    {"unclosed class is kept", "class A\n{\n\tx = 1;\n"},
    {"stray brace is kept", "class A\n{\n};\n}\n"},
};

static QString readFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    return QString::fromUtf8(f.readAll());
}

// Every config.cpp under a root, biggest ones included.
static QStringList configsUnder(const QString &root)
{
    QStringList out;
    QDirIterator it(root, QStringList() << QStringLiteral("*.cpp"), QDir::Files,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) out << it.next();
    out.sort();
    return out;
}

static void countTree(const ConfigClass &c, int *classes, int *values)
{
    (*classes)++;
    *values += c.values.size();
    for (const ConfigClass &k : c.classes) countTree(k, classes, values);
}

// Round trip is the pass or fail. The rest is reporting: how big the corpus is,
// and how much of it landed in the tree rather than being carried as text.
static int corpusRoundTrip(const QString &label, const QStringList &files)
{
    QTextStream o(stdout);
    int ok = 0, bad = 0, empty = 0;
    int classes = 0, values = 0, dirty = 0;
    QStringList broken, complaints;
    QElapsedTimer timer;
    timer.start();
    for (const QString &path : files) {
        const QString text = readFile(path);
        if (text.isEmpty()) { empty++; continue; }
        const ConfigFile f = parseConfig(text);
        if (writeConfig(f) == text) {
            ok++;
        } else {
            bad++;
            QString why;
            roundTrips(text, &why);
            if (broken.size() < 5) broken << QStringLiteral("%1\n           %2").arg(path, why);
        }
        for (const ConfigClass &c : f.classes) countTree(c, &classes, &values);
        values += f.values.size();
        if (!f.errors.isEmpty()) {
            dirty++;
            if (complaints.size() < 5)
                complaints << QStringLiteral("%1 %2").arg(QFileInfo(path).fileName(), f.errors.first());
        }
    }
    const int total = ok + bad;
    o << QStringLiteral("  %1: %2/%3 round trip").arg(label).arg(ok).arg(total);
    if (total > 0) o << QStringLiteral(" (%1%)").arg(100.0 * ok / total, 0, 'f', 1);
    if (empty) o << QStringLiteral(", %1 empty").arg(empty);
    o << QStringLiteral(", %1 ms").arg(timer.elapsed()) << Qt::endl;
    o << QStringLiteral("         %1 classes, %2 properties, %3 file(s) with text the tree "
                        "could not model")
             .arg(classes).arg(values).arg(dirty)
      << Qt::endl;
    for (const QString &b : broken) o << "         " << b << Qt::endl;
    if (verbose) for (const QString &c : complaints) o << "         " << c << Qt::endl;
    return bad;
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTextStream o(stdout);
    const QStringList args = app.arguments();
    verbose = args.contains(QStringLiteral("-v"));
    QString resources;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i).startsWith(QLatin1Char('-'))) continue;
        resources = args.at(i);
        break;
    }
    if (resources.isEmpty()) resources = QStringLiteral("resources");

    o << "hand written corpus" << Qt::endl;
    for (const Case &c : kCases) {
        QString why;
        const QString text = QString::fromUtf8(c.text);
        check(roundTrips(text, &why),
              why.isEmpty() ? QString::fromUtf8(c.name)
                            : QStringLiteral("%1\n         %2").arg(c.name, why));
    }

    o << "the config the user pasted" << Qt::endl;
    const QString userConfig = QString::fromUtf8(kUserConfig);
    {
        QString why;
        check(roundTrips(userConfig, &why),
              why.isEmpty() ? QStringLiteral("round trips")
                            : QStringLiteral("round trips\n         %1").arg(why));
    }

    o << "reading the tree" << Qt::endl;
    {
        ConfigFile f = parseConfig(userConfig);
        check(f.errors.isEmpty(), QStringLiteral("no errors (%1)").arg(f.errors.join(QStringLiteral("; "))));
        check(f.classes.size() == 3, QStringLiteral("three top level classes (%1)").arg(f.classes.size()));

        const ConfigClass *patch = findClass(f, QStringLiteral("CfgPatches/MT_Scripts"));
        check(patch != nullptr, "finds CfgPatches/MT_Scripts");
        const ConfigClass *mod = findClass(f, QStringLiteral("CfgMods/SudoTest3"));
        check(mod != nullptr, "finds CfgMods/SudoTest3");

        ConfigValue *req = findValue(f, QStringLiteral("CfgPatches/MT_Scripts/requiredAddons"));
        check(req && req->isArray && req->items == QStringList{QStringLiteral("\"DZ_Scripts\"")},
              "requiredAddons is an array of one");

        ConfigValue *name = findValue(f, QStringLiteral("CfgMods/SudoTest3/name"));
        check(name && !name->isArray && name->scalar == QLatin1String("\"SudoTest3\""),
              "name is the scalar it was written as");
        check(name && configUnquote(name->scalar) == QLatin1String("SudoTest3"),
              "and unquotes to SudoTest3");

        ConfigValue *deps = findValue(f, QStringLiteral("CfgMods/SudoTest3/dependencies"));
        check(deps && deps->items.size() == 3, QStringLiteral("dependencies has three entries (%1)")
                                                   .arg(deps ? deps->items.size() : -1));

        const ConfigClass *defs = findClass(f, QStringLiteral("CfgMods/SudoTest3/defs"));
        check(defs && defs->classes.size() == 6,
              QStringLiteral("defs holds six script modules (%1)").arg(defs ? defs->classes.size() : -1));

        ConfigValue *engine = findValue(f, QStringLiteral("CfgMods/SudoTest3/defs/engineScriptModule/files"));
        check(engine && engine->items.value(0) == QLatin1String("\"SudoTest3/Scripts/1_Core\""),
              "reaches a leaf four levels down");

        check(patch && mod && patch->name != mod->name,
              QStringLiteral("catches the leftover: CfgPatches says %1, CfgMods says %2")
                  .arg(patch ? patch->name : QString(), mod ? mod->name : QString()));
    }

    // Round trip alone cannot tell a modelled construct from one carried
    // through as text, and both of these are things the Enforce tokeniser
    // splits in two. They have to end up in the tree, not beside it.
    o << "shapes the tokeniser splits" << Qt::endl;
    {
        ConfigFile f = parseConfig(QStringLiteral("class A\n{\n\tvarTemperatureFreezePoint=-1;\n};\n"));
        ConfigValue *v = findValue(f, QStringLiteral("A/varTemperatureFreezePoint"));
        check(v && v->scalar == QLatin1String("-1"), "x=-1 is a property, not leftover text");
        check(f.errors.isEmpty(), "and nothing is left over");
        if (v) v->scalar = QStringLiteral("-2");
        check(writeConfig(f).contains(QLatin1String("varTemperatureFreezePoint=-2;")),
              "and it can be edited");
    }
    {
        ConfigFile f = parseConfig(QStringLiteral("class 1kHz_mono_1s_SoundShader\n{\n\tx = 1;\n};\n"));
        check(f.classes.size() == 1
                  && f.classes.first().name == QLatin1String("1kHz_mono_1s_SoundShader"),
              "a class name may start with a digit");
        check(f.errors.isEmpty(), "and nothing is left over");
    }

    o << "editing writes one line" << Qt::endl;

    // How big the diff hunk is: the lines left over once the matching head and
    // the matching tail are taken off both sides. One edit has to mean one line,
    // and an inserted line must not count the untouched lines it pushed down.
    const auto changedLines = [](const QString &before, const QString &after) {
        const QStringList a = before.split(QLatin1Char('\n'));
        const QStringList b = after.split(QLatin1Char('\n'));
        int head = 0;
        while (head < a.size() && head < b.size() && a.at(head) == b.at(head)) head++;
        int tail = 0;
        while (tail < a.size() - head && tail < b.size() - head
               && a.at(a.size() - 1 - tail) == b.at(b.size() - 1 - tail)) tail++;
        return qMax(a.size() - head - tail, b.size() - head - tail);
    };

    {
        ConfigFile f = parseConfig(userConfig);
        ConfigClass *patch = findClass(f, QStringLiteral("CfgPatches/MT_Scripts"));
        patch->name = QStringLiteral("SudoTest3_Scripts");
        const QString out = writeConfig(f);
        check(out.contains(QLatin1String("\tclass SudoTest3_Scripts\n")), "rename lands in the text");
        check(!out.contains(QLatin1String("MT_Scripts")), "and the old name is gone");
        check(changedLines(userConfig, out) == 1,
              QStringLiteral("rename touched %1 line(s)").arg(changedLines(userConfig, out)));
    }

    {
        ConfigFile f = parseConfig(userConfig);
        findValue(f, QStringLiteral("CfgMods/SudoTest3/author"))->scalar = configLiteral(QStringLiteral("Dillan"));
        const QString out = writeConfig(f);
        check(out.contains(QLatin1String("author=\"Dillan\";")), "scalar edit keeps the tight spacing");
        check(changedLines(userConfig, out) == 1,
              QStringLiteral("scalar edit touched %1 line(s)").arg(changedLines(userConfig, out)));
    }

    {
        ConfigFile f = parseConfig(userConfig);
        findValue(f, QStringLiteral("CfgPatches/MT_Scripts/requiredAddons"))
            ->items.append(QStringLiteral("\"DZ_Data\""));
        const QString out = writeConfig(f);
        check(out.contains(QLatin1String("requiredAddons[] = { \"DZ_Scripts\", \"DZ_Data\" };")),
              "array entry keeps the padding inside the braces");
        check(changedLines(userConfig, out) == 1,
              QStringLiteral("array edit touched %1 line(s)").arg(changedLines(userConfig, out)));
    }

    {
        // The multi line array is the one that would get reflowed by a writer
        // that rebuilds from scratch.
        ConfigFile f = parseConfig(userConfig);
        findValue(f, QStringLiteral("CfgMods/SudoTest3/dependencies"))
            ->items.append(QStringLiteral("\"Core\""));
        const QString out = writeConfig(f);
        check(out.contains(QLatin1String("\t\t\t\"Game\", \"World\", \"Mission\", \"Core\"\n")),
              "multi line array keeps its shape");
        check(changedLines(userConfig, out) == 1,
              QStringLiteral("multi line array edit touched %1 line(s)")
                  .arg(changedLines(userConfig, out)));
    }

    {
        ConfigFile f = parseConfig(userConfig);
        ConfigClass *mod = findClass(f, QStringLiteral("CfgMods/SudoTest3"));
        ConfigValue *added = addValue(*mod, QStringLiteral("credits"));
        added->scalar = configLiteral(QStringLiteral("SUDO"));
        const QString out = writeConfig(f);
        check(out.contains(QLatin1String("credits = \"SUDO\";")), "a new scalar is written");
        check(roundTrips(out), "and the result parses back to itself");
        check(changedLines(userConfig, out) == 1,
              QStringLiteral("adding a property touched %1 line(s)")
                  .arg(changedLines(userConfig, out)));
    }

    {
        ConfigFile f = parseConfig(userConfig);
        ConfigClass *defs = findClass(f, QStringLiteral("CfgMods/SudoTest3/defs"));
        ConfigClass *added = addClass(*defs, QStringLiteral("worldScriptModuleExtra"));
        addValue(*added, QStringLiteral("files"), true)
            ->items.append(QStringLiteral("\"SudoTest3/Scripts/4_World\""));
        const QString out = writeConfig(f);
        check(out.contains(QLatin1String("class worldScriptModuleExtra")), "a new class is written");
        check(out.contains(QLatin1String("files[] = {\"SudoTest3/Scripts/4_World\"};")),
              "with its array");
        check(roundTrips(out), "and the result parses back to itself");
    }

    {
        ConfigFile f = parseConfig(userConfig);
        ConfigClass *mod = findClass(f, QStringLiteral("CfgMods/SudoTest3"));
        check(removeValue(*mod, QStringLiteral("picture")), "removes a property");
        const QString out = writeConfig(f);
        check(!out.contains(QLatin1String("picture=")), "and it is gone from the text");
        check(roundTrips(out), "and the result parses back to itself");
    }

    {
        // Nothing touched has to give back the same bytes even after the tree
        // has been walked and read.
        ConfigFile f = parseConfig(userConfig);
        ConfigClass *mod = findClass(f, QStringLiteral("CfgMods/SudoTest3"));
        for (const ConfigValue &v : mod->values) Q_UNUSED(v);
        check(writeConfig(f) == userConfig, "reading the tree changes nothing");
    }

    o << "repo corpus" << Qt::endl;
    {
        const QDir res(resources);
        QStringList files;
        const QString tmpl = res.filePath(QStringLiteral("mod-template/ModTemplate/Scripts/config.cpp"));
        if (QFile::exists(tmpl)) files << tmpl;
        files += configsUnder(res.filePath(QStringLiteral("mod-template/Missions")));
        check(!files.isEmpty(), QStringLiteral("found configs under %1").arg(res.absolutePath()));
        if (verbose) for (const QString &f : files) o << "         " << f << Qt::endl;
        const int bad = corpusRoundTrip(QStringLiteral("resources"), files);
        check(bad == 0, QStringLiteral("every repo config round trips (%1 failed)").arg(bad));

        for (const QString &path : files) {
            const ConfigFile f = parseConfig(readFile(path));
            if (!f.errors.isEmpty() && verbose)
                o << "         " << QFileInfo(path).fileName() << ": "
                  << f.errors.join(QStringLiteral("; ")) << Qt::endl;
        }
    }

    // The work drive is where the real corpus lives: the vanilla configs under
    // P:\DZ plus whatever mods the machine has unpacked, none of them written
    // with this parser in mind. Skipped without complaint when P: is not
    // mounted, the same way the lexer test treats vanilla source.
    o << "work drive corpus" << Qt::endl;
    {
        const QString root = QStringLiteral("P:/");
        if (!QDir(root).exists()) {
            o << "  P: is not mounted, skipping" << Qt::endl;
        } else {
            const QStringList files = configsUnder(root);
            o << QStringLiteral("  %1 files").arg(files.size()) << Qt::endl;
            const int bad = corpusRoundTrip(QStringLiteral("work drive"), files);
            check(bad == 0, QStringLiteral("every work drive config round trips (%1 failed)").arg(bad));
        }
    }

    o << (fails ? QStringLiteral("FAILED: %1").arg(fails) : QStringLiteral("all good")) << Qt::endl;
    return fails ? 1 : 0;
}
