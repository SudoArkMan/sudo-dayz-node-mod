// Node templates: nodes defined by data rather than by code.
//
// A template is a title, typed pins and an Enforce snippet carrying `{pin}`
// placeholders. The Electron build keeps the mechanism in nodes/templates.ts
// and the shipped set in nodes/builtin-sets.ts; both are mirrored here because
// a project authored there stores `tpl.*` node refs, and this build has to
// generate the same script for them rather than dropping the statement.
//
// The table exists so a set can later be loaded from a file without touching
// the generator: only the data changes.
#pragma once

#include "graph.h"

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

struct TemplatePin {
    QString id;
    QString label;
    QString type;  // Enforce type name; "exec" is not used by the shipped set
    PinDir dir = PinDir::In;
};

struct NodeTemplate {
    QString id;
    QString title;
    QString category;
    QString subtitle;
    QString summary;
    QStringList cautions;
    // No exec pins; the snippet is an expression rather than a statement.
    bool pure = false;
    QVector<TemplatePin> pins;
    QString code;
    bool valid = false;
};

extern const QString TEMPLATE_PREFIX;

bool isTemplateKey(const QString &key);

// Every template the app ships with.
const QVector<NodeTemplate> &builtinTemplates();

// Invalid template when the key names one that does not exist.
NodeTemplate findTemplate(const QString &key);

NodeDef templateDef(const NodeTemplate &t,
                    const std::function<bool(const QString &)> &isEnum);

// Substitutes pin expressions into the snippet, left to right. An unknown
// placeholder is left alone rather than blanked, so a typo in a template shows
// up in the output instead of silently producing `null`.
QString renderTemplate(const NodeTemplate &t,
                       const std::function<QString(const QString &)> &resolve);
