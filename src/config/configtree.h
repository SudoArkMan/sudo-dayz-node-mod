// Bohemia config.cpp as a class tree.
//
// config.cpp is not code. It has no execution order and nothing flows between
// its parts: it is a nested tree of classes holding properties and arrays. That
// is why it gets a tree with a property panel rather than a node canvas, and
// why this file is a model instead of a graph.
//
// The parser reuses the Enforce tokeniser, so a `//` inside a quoted path stays
// part of the path instead of eating the rest of the line. Anything it cannot
// read is carried through verbatim.
//
// Every construct also remembers the text it was parsed from. writeConfig
// reuses that text unless the meaning changed, which buys two things: opening a
// file and saving it gives back the same bytes, and one edit rewrites one line
// instead of reformatting somebody's mod config.
#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

#include <climits>

// Source formatting for one property, captured at parse time. Nothing here
// changes what the property means; it exists so writing back looks like what
// the modder typed.
struct ValueFormat {
    QString lead;      // trivia between the previous member and this one
    QString head;      // "requiredAddons[] =", operator included
    QString body;      // everything between the operator and the ';'
    QString tail;      // ";"
    QString pre;       // whitespace inside body, before the value
    QString post;      // whitespace inside body, after the value
    QString padOpen;   // whitespace just inside '{'
    QString padClose;  // whitespace just inside '}'
    QString sep;       // text between two entries, the comma included
    bool braced = false;  // the value really is a { ... } list
    QString sigHead;   // meaning of the head when it was parsed
    QString sigBody;   // meaning of the body when it was parsed
};

// A property: scalar or array.
struct ConfigValue {
    QString name;              // "requiredAddons", "name", "files"
    bool isArray = false;      // written as name[] = { ... }
    QString scalar;            // the literal as written, quotes included
    QStringList items;         // array entries, each as written
    QString op = QStringLiteral("=");  // "=" or "+=" as it was written
    int line = 0;              // 1 based, for error reporting
    int seq = INT_MAX;         // order among its parent's members
    ValueFormat fmt;
};

// Source formatting for one class. Same deal as ValueFormat.
struct ClassFormat {
    QString lead;         // trivia between the previous member and this one
    QString headPrefix;   // "class X : Y"
    QString headSuffix;   // "\n\t{" for a block, ";" for an external
    QString bodyTail;     // trivia between the last member and '}'
    QString footer;       // "};"
    QString indent;       // indentation of the line the class starts on
    QString childIndent;  // indentation its members sit at
    QString sigHead;      // meaning of the header when it was parsed
};

struct ConfigClass {
    QString name;
    QString base;              // class X : Y  (config inheritance)
    bool external = false;     // a bare "class X;" forward declaration
    QVector<ConfigValue> values;
    QVector<ConfigClass> classes;
    int line = 0;
    int seq = INT_MAX;
    ClassFormat fmt;
};

struct ConfigFile {
    QVector<ConfigClass> classes;
    QVector<ConfigValue> values;   // top level properties, as in P:\bin\config.cpp
    QStringList errors;
    QString preamble;              // #include and leading comments
    QString trailer;               // trivia after the last construct
    QString newline = QStringLiteral("\n");  // what this file uses, for new members
};

ConfigFile parseConfig(const QString &text);
QString writeConfig(const ConfigFile &file);

// Lookup. Paths are slash separated class names, "CfgMods/SudoTest3/defs" for a
// class and "CfgMods/SudoTest3/name" for a property. The first match wins.
ConfigClass *findClass(ConfigFile &file, const QString &path);
const ConfigClass *findClass(const ConfigFile &file, const QString &path);
ConfigClass *findClass(ConfigClass &parent, const QString &path);
ConfigValue *findValue(ConfigClass &parent, const QString &name);
ConfigValue *findValue(ConfigFile &file, const QString &path);

// Insertion. Each returns a pointer into the parent's vector, so it goes stale
// as soon as the same parent gains another member. Read it, use it, drop it.
ConfigValue *addValue(ConfigClass &parent, const QString &name, bool isArray = false);
ConfigClass *addClass(ConfigClass &parent, const QString &name, const QString &base = QString());
ConfigClass *addClass(ConfigFile &file, const QString &name, const QString &base = QString());

bool removeValue(ConfigClass &parent, const QString &name);
bool removeClass(ConfigClass &parent, const QString &name);

// config.cpp has no escape for a double quote inside a string, so one in the
// text becomes an apostrophe rather than a config the engine refuses to load.
// This matches what the mod scaffolder already does.
QString configLiteral(const QString &text);
// "SudoTest3" back to SudoTest3. Leaves anything unquoted alone.
QString configUnquote(const QString &literal);
