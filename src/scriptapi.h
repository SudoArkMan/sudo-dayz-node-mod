// Nodes for the scripts in *this project*, as opposed to the vanilla catalogue.
//
// A script's own functions and variables are part of its API. Once one script
// holds a reference to another, you want to call into it:
//
//   fn.entry.<fnId>            the body of one of this script's functions
//   fn.call.<scriptId>.<fnId>  call a function on a target of that class
//   sv.get.<scriptId>.<varId>  read a member off a target
//   sv.set.<scriptId>.<varId>  write a member on a target
//
// Keys carry ids rather than names so renaming a script or member does not
// orphan every node that referenced it.
#pragma once

#include "graph.h"
#include "project.h"

#include <functional>

using IsEnumFn = std::function<bool(const QString &)>;

QString functionSignature(const GraphFunction &f);

NodeDef functionEntryDef(const GraphFunction &f, const IsEnumFn &isEnum);
NodeDef functionCallDef(const ScriptEntry &s, const GraphFunction &f,
                        const IsEnumFn &isEnum);
NodeDef scriptVarGetDef(const ScriptEntry &s, const GraphVariable &v,
                        const IsEnumFn &isEnum);
NodeDef scriptVarSetDef(const ScriptEntry &s, const GraphVariable &v,
                        const IsEnumFn &isEnum);

// Resolve any project-script key. A project can carry a node whose target was
// deleted, so a bad key degrades to an invalid def rather than crashing.
NodeDef scriptDefFor(const QString &key, const Project &project,
                     const IsEnumFn &isEnum);

bool isScriptNodeKey(const QString &key);

// Raw records the code generator needs.
struct CallTarget {
    const ScriptEntry *script = nullptr;
    const GraphFunction *fn = nullptr;
    bool valid = false;
};
CallTarget resolveCall(const QString &key, const Project &project);

struct EntryTarget {
    const GraphFunction *fn = nullptr;
    bool valid = false;
};
EntryTarget resolveEntry(const QString &key, const Project &project);

struct MemberTarget {
    bool setter = false;
    const GraphVariable *variable = nullptr;
    const ScriptEntry *script = nullptr;
    bool valid = false;
};
MemberTarget resolveMember(const QString &key, const Project &project);

// Every project-script node available for the palette.
QVector<NodeDef> projectApiDefs(const Project &project, const IsEnumFn &isEnum);
