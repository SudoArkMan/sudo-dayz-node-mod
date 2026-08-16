# Custom nodes, rules, and node packs

**None of this is built.** It is a design note for work that is planned, kept in the repository
because the measurements it rests on are real and because the shape of the design is the argument.
Nothing described below is in the 0.1.0 release, and the file names and line numbers it cites were
true when it was written rather than being kept current.

Three things are wanted: **author a node**, **give it rules**, **share it**. They are not equally
missing, and the design is shaped by which one is actually the gap.

---

## The one-paragraph version

A custom node is a `GraphFunction` with a face on it. You make one by selecting nodes that already
work and pressing a key, which turns them into a method plus a call. You share one by putting its
body in a pack file, and placing a pack node **stamps** a copy of that body into your script as an
ordinary function. Nothing in the generated `.c` ever depends on a file outside the mod folder, so
a project that used a pack opens and generates identically on a machine that has never seen the
pack. A pack body is a graph of catalogue keys and carries no Enforce text at all, which is what
turns the trust story from a promise into a property. Rules are a closed list of nine predicates
that run inside `analyzeGraph` on every edit, in the same pass, with the same badges.

---

## What already exists, measured rather than remembered

### `GraphFunction` is already a custom node

`src/graph.h:118-132` gives a named thing with typed `params`, a return type, access flags and a
body. `src/scriptapi.cpp:73-107` builds its call def, `src/scriptapi.cpp:55-71` its entry def,
`src/codegen.cpp:2163-2239` writes the method, `src/analysis.cpp:645-664` resolves both ends. The
user can have one today from Add custom event (`src/mainwindow.cpp:2170-2221`).

Three things it does not give, and the list is short on purpose:

1. **You cannot make one out of work you already did.** The gesture today is "declare an empty
   method, then rebuild the graph inside it". That is the whole of the authoring request and it is
   the cheapest of the three to satisfy.
2. **It cannot leave the project.** `fn.call.<scriptId>.<fnId>` carries a project-local script id
   (`src/scriptapi.h:6-8`) and there is no shape in the format for a definition that came from
   outside.
3. **It carries no documentation and no rules.** `NodeDef::doc` exists and `functionCallDef` never
   fills it (`src/scriptapi.cpp:100-105`).

So the honest sentence, and it should appear in the UI: *a pack node is a function that left the
project, with documentation and rules attached.*

### The template mechanism is built and half wired, and that is a standing bug

`NodeTemplate` (`src/templates.h:21-40`) is a data-driven node definition with pins. `templateDef`
turns it into a `NodeDef` and generates exec pins for non-pure nodes (`src/templates.cpp:250-275`).
`renderTemplate` substitutes `{pin}` holes left to right (`src/templates.cpp:277-298`).

**Sixteen** templates ship, not seventeen. Counted from the `add({` calls at
`src/templates.cpp:34-207`: `str.concat2`, `str.append`, `str.format`, `obj.member`,
`obj.setMember`, `arr.new`, `arr.count`, `arr.get`, `arr.insert`, `arr.clear`, `json.load`,
`json.save`, `file.exists`, `timer.callLater`, `timer.remove`, `misc.methodRef`.

`isTemplateKey`, `findTemplate` and `templateDef` are referenced in exactly one consumer:
`src/codegen.cpp:448`, `:928`, `:1791`. Nowhere else in the tree. Three consequences, all checkable:

- `Document::defForKey` (`src/document.cpp:16-27`) tries builtins, then project script keys, then
  the catalogue. None of the three knows the prefix, so a template node resolves to an invalid def
  and the canvas draws it as its raw key with the subtitle `unknown node` and **no pins at all**
  (`src/canvas/nodeitem.cpp:351-356`; pins come from `m_def.pins`).
- `resolveDef` (`src/analysis.cpp:632-667`) routes a node of kind Builtin to
  `builtins.defForNode`, which has no template table, so the analyser sees nothing.
- `src/enforce/lower.cpp` never produces one, so a template node has a write path and no read path.

Meanwhile `codegen.cpp` resolves it properly and emits correct code. An Electron-authored project
opened here is a wall of unwireable boxes that nonetheless generates a working `.c`. Fixing this is
smaller than every other item in this document and worth more than most of them.

### Eight of the sixteen templates generate code that does not compile

A template pin declared `"string"` becomes `PinKind::String` (`src/templates.cpp:219` calling
`pinTypeOf`, `src/pins.cpp:138`), and an unconnected literal on a String pin is quoted at emit
(`src/codegen.cpp:1585-1588`). So `obj.member`, whose code is `{obj}.{field}` with `field` typed
`string` (`src/templates.cpp:75`), generates `player."m_Health"`.

The same fault hits `obj.setMember` (`:85`), `arr.new` (`new array<"ref X">()`, `:96`), `json.load`
and `json.save` (`JsonFileLoader<"MyClass">`, `:147`, `:158`), `misc.methodRef` (`this."Method"`,
`:205`), and both timing templates (`:183`, `:195`), where `timer.callLater`'s own caution says the
argument takes a function reference with no quotes and the template then quotes it. It only bites on
typed-in literals, not on wired pins, which is exactly how those pins are meant to be used.

**The fix is not a new format field.** `PinKind::Typename` already exists, `pinTypeOf` already maps
the Enforce type name `typename` onto it (`src/pins.cpp:64`), `literal()` already falls through to
`return v` unquoted for it (`src/codegen.cpp:1598`), `inlineEditorFor` already gives it a text field
(`src/pins.cpp:170`) and it already has its own pin colour (`src/pins.cpp:22`). Retyping three pins
is a one-word data change per pin.

The other five are not types, they are identifiers, and the repo has already decided where an
identifier that names something in the generated code lives: in **opts**, not in a pin. The Timer
node reads its name from `opts["name"]` through `bi::timingName` (`src/builtins.h:139-143`), never
from a pin. Those five pins move to opts, edited in the inspector, with the same
`^[A-Za-z_]\w*$` shape check that already guards a custom event name
(`src/mainwindow.cpp:2188-2193`).

This is the compatibility-safe moment to do it, because nothing can depend on output that does not
compile.

### The Timer is the worked example of a composite, and it is the ceiling

`bi.setTimer` emits a `ref Timer` member, a constructor call, a `Run` call and a whole callback
method carrying the `elapsed` chain: `src/codegen.cpp:948-959` for the statements,
`src/codegen.cpp:2241-2326` for the member and the method. It needs `takenMethods` and
`takenMembers` built from every declared function and variable (`src/codegen.cpp:2260-2263`), two
collision refusals (`:2273`, `:2282`), a name-derivation triple (`bi::timingName`, `bi::timerMember`,
`bi::timerCallback`, `src/builtins.h:139-148`), and four diagnostics that exist only to police it:
DZ317 (`src/analysis.cpp:1844`), DZ318 (`:1895`), DZ319 (`:1933`), DZ320 (`:1862`).

That is roughly 200 lines across three modules for **one** node. The reason it has to be one
decision is in the API: `Timer::Run(float duration, Managed obj, string fn_name, Param params = NULL,
bool loop = false)` (`3_game/tools/tools.c:595`) dispatches on a **string**, so the string and the
method it names have to be derived from the same place or they drift silently. `ScriptCallQueue`
takes the other shape, `CallLater(func fn, int delay = 0, bool repeat = false, ...)`
(`2_gamelib/tools.c:61`), which is why the template's caution about quoting is right and why the
template itself is wrong.

**A design that lets users author composites is a design that lets users author that failure mode.**
This one does not.

---

## The model

### A custom node is a function, and a pack node is a stamped function

Three scopes, and they differ in one way.

| Scope | Where the definition lives | Placing it again is |
| --- | --- | --- |
| This script | this graph's `functions` | a reference |
| This project | another script's graph, via `fn.call.<otherId>.<fnId>` | a reference |
| A pack | a `.sdznpack` outside every project | a **stamp**, copied in |

Project scopes are references. A pack is a stamp. That asymmetry is the load-bearing decision.

The `.c` file is what ships. A reference to a definition outside the project means the generated
script depends on a file the mod folder does not carry, and the first regenerate on a machine
without the pack produces a mod calling a method nobody wrote. The repo already treats a link that
can go missing as a link that will: a dependency's `scriptRoot` is stored relative to the `.sdzn` and
the project still knows what it depends on when the mod is absent (`src/project.h:64-70`,
`src/moddeps.h:49-54`).

Stamping pays for itself three times over:

- **The round trip survives in both directions.** A stamped node emits a method plus a call, which is
  the shape `src/enforce/lower.cpp` already produces and `src/codegen.cpp:2163-2239` already writes.
  Import that file and you get a `GraphFunction` and a `fn.call` node back.
- **A missing pack is not an error.** The function is in the graph. Generation is unchanged. What is
  missing is the pack's title, its documentation and its rules, which is a banner, not a refusal.
- **No new node key enters the `.sdzn`.** A stamped node is an ordinary `fn.call`. The only format
  change is one optional object on `GraphFunction::extra`, which is preserved by both builds
  (`src/graph.h:131`, `DESIGN.md:8-11`).

The cost, stated plainly: **a pack is a copy, so a bug never gets fixed by fixing the pack.** Ship a
pack, someone stamps it into four scripts across two mods, find the bug, fix the pack: nothing
changes anywhere until a human runs Update from library on each stamped function. There is no way to
find which of your own mods carry the broken version except opening them one at a time. That is the
price of not letting the generated `.c` depend on a file outside the mod, and it is a real price.

### Collapse to node: the authoring gesture

Select nodes, press `Ctrl+Shift+C` or pick `Collapse to node` from the canvas menu. The selection is
replaced by one node at its centroid and a new function appears in the Variables panel with the caret
in its name field, the way promoting a wire to a variable already hands the caret over
(`src/mainwindow.cpp:2236-2237`).

Nothing is authored. No snippet, no placeholder syntax, no schema. **The pins the user gets are the
wires that were already there**, which is why this has the lowest authoring burden of anything
considered and why it is stage one.

Wires are classified by which side of the selection boundary each end sits on.

**Exec in.** An exec wire from an unselected output into the selection becomes the call node's exec
input. Exactly one, or none.

**Exec out.** An exec wire from inside the selection to an unselected input becomes the exec output.
At most one. This is the rule that does most of the refusing, and it is not arbitrary: a method call
returns to one place.

**Data in.** A wire from outside into the selection becomes a parameter, one per **distinct upstream
pin**, not one per wire. Two inputs fed by the same Get Health node become one parameter wired to
both places inside. Parameter type is the upstream pin's type verbatim, so the pin the user sees on
the new node is the pin they were already wiring into.

**Data out.** A wire from inside the selection to an outside input becomes the return value. Exactly
one, or none.

**Unconnected literal inputs stay inside as literals.** They do not become pins. A collapse that
turned every typed-in `5.0` into a parameter would produce a node with eleven pins on first use.
Promoting one afterwards is a right-click on the pin inside the function, and it is the same
operation as adding a parameter.

**Pure nodes read from both sides are copied, not moved.** A pure node is an expression; duplicating
it duplicates no work, and the alternative is a parameter carrying a value the function could compute
itself. This is the one case where the node count changes by more than the user selected, so the
status line says so:

> Collapsed 9 nodes into Refuel. 2 pure nodes were copied because the graph outside still reads them.

**Naming decides whether the node has exec pins.** `functionCallDef` builds a pure node when the
function returns a value and is either static or named with a Get/Is/Has/Can/To/From prefix
(`src/scriptapi.cpp:78-82`). A collapsed selection that returns a value and is named `Damage` gets
exec pins; named `GetDamage` it does not. That is existing behaviour and it is a reasonable
convention, but the rename dialog has to say it in one line or it reads as a bug:

> Names starting with Get, Is, Has, Can, To or From make a node with no exec pins.

### What cannot be collapsed

Each refusal names a count or a node, because a refusal the user cannot act on is a refusal they work
around by not using the feature.

| Refusal | Message |
| --- | --- |
| An event node in the selection | `An event node is where the flow starts, so it cannot move inside a node. Select the nodes after it.` |
| A Return node in the selection | `A Return node inside a custom node returns from the node, not from the event. Leave it outside the selection.` |
| More than one exec wire leaving | `The flow leaves this selection in 3 places. A node has one way out, so collapse needs the selection to have one too.` |
| More than one data wire leaving | `Two values leave this selection. A node hands back one. Wire the second one out separately, or select less.` |

The Return refusal is the only one preventing a correctness bug rather than a compile error. A
`return` that used to bail out of `EEHitBy` would bail out of the helper and the event would carry
on. Nothing in the generated file would look wrong.

Stated formally, all four are one rule: the selection must be a single-entry, single-exit region of
the exec graph. Taking a Branch's `true` subtree without the Branch produces more than one crossing
and is caught. Saying it as one sentence up front is clearer than letting the user find it as four.

The one-data-output limit is not a language limit. Enforce does multiple returns with `out`
parameters and vanilla does it constantly:
`EntityAI::GetColor(out int r, out int g, out int b, out int a)` at `3_game/entities/entityai.c:2741`,
`World::GetDate(out int year, out int month, out int day, out int hour, out int minute)` at
`3_game/global/world.c:33`. It is a limit in this repo: `GraphParam` is `{name, type}` with no
direction (`src/graph.h:112-115`), and `functionCallDef` builds every param as `PinDir::In` with only
`ret` as an output (`src/scriptapi.cpp:93-98`). Lifting it means a `dir` field on `GraphParam` plus
matching changes in `functionSignature`, `functionEntryDef`, `functionCallDef` and the argument
builder at `src/codegen.cpp:1774-1777`. Real work, worth doing, not first.

### Expand, the inverse

`Expand` splices the body back inline and deletes the function. **Refused above one caller**, because
expanding one of three call sites has to copy the body and now three copies drift. That means the
natural undo of a collapse stops working as soon as the node is placed a second time, which users
will read as a bug. It is stated in the refusal:

> Refuel is called from 3 places. Expanding one would copy the body into all of them, and they would
> drift apart. Delete the other calls first.

---

## The pack format

One JSON file, one pack, extension `.sdznpack`. No archive: a pack has to travel as a chat
attachment and be readable in a text editor by whoever reviews it, and there is no zip in Qt core
anyway.

```json
{
  "format": "sdznpack",
  "formatVersion": 1,
  "pack": {
    "id": "server-chores",
    "name": "Server chores",
    "author": "sudo",
    "version": "1.2.0",
    "summary": "Nodes for the admin jobs every server mod ends up writing."
  },
  "needs": { "addons": ["JM_CF_Scripts"], "app": "0.9.0" },
  "nodes": [
    {
      "id": "refuel",
      "title": "Refuel vehicle",
      "category": "Vehicles",
      "subtitle": "tops a car up and logs it",
      "summary": "Fills the fuel tank and writes a line to the admin log.",
      "cautions": ["Server only. The client has no authority over fuel."],
      "signature": {
        "name": "Refuel",
        "returns": "bool",
        "params": [
          { "name": "car",    "type": "CarScript" },
          { "name": "litres", "type": "float" }
        ]
      },
      "body": { "nodes": [], "edges": [], "variables": [] },
      "rules": [
        {
          "id": "no-car",
          "rule": "pinWired", "pin": "p0",
          "severity": "warning",
          "message": "Nothing is wired to \"car\", so this runs against nothing.",
          "hint": "Wire the vehicle in, or use the Cast To node above it."
        }
      ]
    }
  ]
}
```

Four things about this shape are load bearing.

**`body` is a `Graph`.** Same object, same reader: `graphFromJson` and `graphToJson`
(`src/graph.h:283-284`) do the work, and `repairGraph` and `scrubLayoutKeys` in `src/project.cpp`
already run over one. There is no second parser to keep in step with the first, and no second place
for a validation rule to be forgotten.

**The five help fields are `NodeTemplate`'s five.** `title`, `category`, `subtitle`, `summary`,
`cautions` (`src/templates.h:28-36`) map onto `NodeDef::doc` and `NodeHelp`
(`src/catalog.h:93-101`), so the inspector shows a pack node the way it shows a vanilla one with no
new panel, and `nodeSummary`/`nodeCautions` (`src/nodeindex.h:114-119`) answer for a pack key
without a second code path.

**The palette key is an action key, not a node key.** `pk.<packId>.<nodeId>` names a palette row
whose effect is "stamp this function into the active script and place a call to it". It never
reaches a `.sdzn`. The precedent is `nodeindex::BrowseEventsKey` (`src/nodeindex.h:63`) and the
promote key at `src/mainwindow.cpp:2233`: rows that do something other than place a node by key
already exist. `packId` is `[a-z0-9-]{2,32}` and `nodeId` is `[a-z0-9_.-]+`, so the split is on the
first two dots, the same discipline as `splitCallKey` (`src/analysis.cpp:98-110`).

**Two body kinds, and only one is available to third parties.**

- `body` (a graph) is the only kind a third-party pack may use. Stamped as a function.
- `code` (a `{pin}` snippet) is the existing `NodeTemplate`, legal **only in the built-in pack**. Not
  stamped, resolved live, exactly as it works today. It exists so the sixteen shipped templates are
  expressible in the format, and for shapes where a helper call would be worse code than `a + b`.

A third-party pack carrying a `code` body fails to load, with the node named. A snippet cannot
round-trip and a snippet is text, and text is the hazard class this design removes.

### Provenance, on the stamped function

```json
"extra": {
  "pack": {
    "id": "server-chores",
    "node": "refuel",
    "version": "1.2.0",
    "bodyHash": "sha256:9f2c...",
    "stamped": "2026-08-16"
  }
}
```

A few dozen bytes on `GraphFunction::extra` (`src/graph.h:131`). It is what `Update from library`
reads, what DZ402 compares against, what puts the badge on the node, and what writes one comment line
above the stamped method in the generated `.c`. That last piece is the one that survives leaving the
app: somebody reviewing a pull request sees where the method came from without opening the tool.

Unknown JSON fields are preserved on load and written back on save (`src/graph.h:9-11`,
`src/project.h` header), so this key survives a round trip through the Electron build untouched.

### Update from library

A stamped function whose pack has a different `bodyHash` offers `Update from library`. It shows the
signature diff first, then re-stamps the body. **Never automatic**, and refused outright when the
function was edited by hand since stamping, which the hash detects.

---

## Rules

"Establish rules" has three answers and only one of them is new.

### Tier one: the pins, which the user gets without asking

A pin's type is the strongest rule in the app, because `canConnect` (`src/graph.h:259-262`) fires
before the mistake exists rather than after. Collapse hands a custom node its full type surface for
free: the parameter types are the types of the wires that were already there.

**There is a real chance this is most of what the user meant**, and it ships in stage one without a
line of rule machinery. That is worth finding out before building tier three.

### Tier two: dependency rules, which are plumbing rather than rules

`dependencyOf` (`src/analysis.cpp:2263-2277`) reads an addon id out of a `dep.` key and looks it up
in `DependencyContext::deps`. Give it a second branch that reads `needs.addons` off the stamped
function's `extra.pack`, and add the enabled packs' needed addons to the context as `ModDependency`
entries filled from `knownDependency()` (`src/moddeps.h:78`). Three rules then cover pack nodes with
no rule authored at all:

- **DZ314** (`src/analysis.cpp:2333`): this graph uses a pack that needs `JM_CF_Scripts`, and the
  mod's `CfgPatches` does not list it.
- **DZ315** (`:2357`): that dependency is optional and nothing sits behind the `#ifdef`.
- **DZ316** (`:2384`): the dependency has requirements of its own this mod does not declare.

This is about ten lines and it is the cheapest real win in the whole document.

### Tier three: nine authored rules, a pick-list and not a language

Every rule is `{ "id", "rule", <its own fields>, "severity", "message", "hint" }`, which is the shape
`Diagnostic` already carries (`src/analysis.h:35-42`). The editor is a row of dropdowns rendering one
English sentence. The predicate list is closed.

| `rule` | Fields | Fires when | Machinery it reuses |
| --- | --- | --- | --- |
| `pinWired` | `pin` | no edge into the pin and no literal other than the default | `edgeInto`, `src/graph.h:275`; DZ101 shape, `src/analysis.cpp:787` |
| `pinNotValue` | `pin`, `value` | the literal on that pin equals `value` | reads `node.inputs` |
| `serverSide` | none | not reachable from a server guard | the seed walk in `missingServerGuard`, `src/analysis.cpp:1669-1712`, DZ303 at `:1724` |
| `clientSide` | none | the mirror of the above | same |
| `afterNode` | `key` | no node with that key upstream on the exec chain | `reachesAny`, `src/analysis.cpp:1533-1546` |
| `notAfterNode` | `key` | the reverse | same |
| `notInLoop` | none | the node is inside a loop body | `flowFrom(loop, "body")`, `src/analysis.cpp:620-628`, used by DZ307 at `:2038` |
| `atMostOne` | none | two stamps of this pack node in one graph | the `claims` count behind DZ320, `src/analysis.cpp:1861` |
| `selfClassIsA` | `class` | `selfClassOf(graph)` does not descend from it | `Catalog::isA`, `src/catalog.h:133`, DZ308 at `:2073` |

`serverSide` is the strongest evidence this tier is not speculative: the guard walk is already
implemented and its only hard-coded part is the subject list, `serverOnlyMethods()` at
`src/analysis.cpp:1655-1667`. The rule engine is not new work, it is one existing pass reading its
subjects from data instead of from a `QSet<QString>` literal.

### Rules about rules

These four are what stop a stranger's file from taking over the status bar, and they are the part of
this section most likely to be dropped under time pressure. They should not be.

1. **A rule's subject is implicit: the node it is declared on.** A pack cannot write a rule about
   anybody else's node, so a graph using none of its nodes hears nothing from it.
2. **Severity is capped at warning or info. Never error.** A pack cannot know that somebody else's
   mod will not compile, and a pack marking its house style as an error makes the status bar useless
   downstream. This is enforced at load, not documented in a style guide.
3. **No suppression field.** A pack cannot turn off a DZ rule. The DZ rules are this tool's own claim
   about DayZ and a stranger's file must not be able to retract one in your project. A pack that
   disagrees says so in a `cautions` string.
4. **A rule that cannot fire is impossible by construction.** Every rule names a pin or a key, and
   the installer validates each one against the node's own declared pins. A rule pointing at a pin
   the node does not have means the pack does not install, and the message says which rule and which
   pin. Budget on top: at most eight rules per node, one finding per rule per node per run, and a
   per-pack ceiling per graph after which one line reads `and 4 more from Server chores`.

### When they run, and what the user sees

Inside `analyzeGraph`, on the same pass as everything else, on every graph edit. Same status bar
count, same canvas badge (`DESIGN.md:96-97`), same inspector text.

The rules reach the analyser through a new field on `DependencyContext` (`src/analysis.h:63-78`)
rather than a signature change. That struct already exists for exactly this purpose: facts the
analyser cannot read for itself because it opens no files, which the caller reads once. A
default-constructed context means no packs and no findings, so every existing caller keeps compiling
and keeps its current output (`src/analysis.h:86-91`).

Rule ids are `pk.<packId>.<ruleId>`, which cannot collide with a `DZ###` code. A finding the user did
not write has to say who is asking, so the pack name is in the message:

> Refuel vehicle: nothing is wired to "car", so this runs against nothing. From pack "Server chores".

Pack findings sort after the built-in families at equal severity, so a pack cannot bury a DZ error
under its own noise.

### What cannot be expressed, said where a rule is added

Anything about a runtime value. Anything about another file in the mod, including `config.cpp`
classes, `types.xml` entries and mission files, because the analyser opens no files and
`DependencyContext` exists to keep it that way. Anything across scripts, because `analyzeGraph` is
handed one graph and never the project (`src/analysis.h:88-91`). Anything the catalogue does not
know.

The rule most people want, "this item has to be registered in types.xml or nothing spawns", cannot be
written. The authoring dialog lists these limits beside the rule picker rather than letting somebody
discover them by writing a rule that never fires.

---

## Trust

A pack emits code into somebody else's mod. This is the sharpest edge in the design and it gets the
most specific answer.

### The two sentences that carry it

**A pack body is a graph of catalogue keys. A pack cannot ship Enforce text.**

`bi.raw`, `bi.rawExpr` and `bi.comment` are the three refs whose text the generator pastes verbatim
(`DESIGN.md:55-58`). **A pack containing one is refused at load**, not stripped and not warned about,
with the count and the first node named. A pack author who needed raw Enforce should have shipped a
mod.

Removing text is what makes the rest cheap. There is no template hole to sit inside a string literal,
no brace depth to go negative, no minimum-depth field to add to `EnforceScan` (which today reports
only net `braceBalance`, `src/enforce/lexer.h:66`, so `Print(1); } void Evil() {` would net to zero
and pass). None of that machinery has to exist, because none of that input shape is accepted.

The repo already draws this line for a much weaker case. `nodefmt::isValidValue`
(`src/graph.h:213-221`) refuses anything but whitespace and comments in the layout keys, and
`src/graph.h:215-221` says why in as many words: a `trivia.before` holding
`GetGame().RequestExit(0);` would otherwise reach the user's mod as code. It is checked where a
`.sdzn` is read and again where a `.c` is written (`src/codegen.cpp:189-199`), and `tests/coretest.cpp:417-452`
proves end to end that a hostile project generates a script with none of its hidden code in it while
the rest of the method still generates.

**Take both habits, not just the predicate.** Every structural check runs where a pack is loaded and
again where a body is stamped, because a `.sdznpack` can be edited after install. And the failure
mode is the established one: refuse the node, put the reason in the warnings, keep generating the
rest of the file. Refusing the whole file takes a working script away from somebody whose only fault
was opening a project.

### What refusing text does not close

**A pack can still call anything in the catalogue.** `GetGame().RequestExit(0)` has a real key and a
node graph can express it. `CGame::RequestExit(int code)` is at `3_game/global/game.c:217`. So the
second half is disclosure, not prohibition.

- **Installed is not enabled.** A pack contributes nothing to the palette and runs no rule until the
  user turns it on. Per pack, per machine. This mirrors the mod browser's stance, which is read only
  without exception and writes every extraction into the app's own cache
  (`src/modlibrary.h:18-21`).
- **Before enabling, the user sees what it calls.** Not a wall of nodes: the distinct catalogue
  methods across every body in the pack, with the ones that matter lifted to the top. It is
  computable without running anything, because a body is a list of catalogue keys, which is the
  second payoff of the no-text decision.

  > This pack has 12 nodes calling 31 methods. 2 of them write files. 1 ends the game process.

  The lifted list, verified against the vanilla index: `CGame::RequestExit`
  (`3_game/global/game.c:217`), `ScriptModule::LoadScript` (`1_core/proto/enscript.c:160`),
  `ScriptModule::CallFunction` and `CallFunctionParams`, `EnScript::GetClassVar` and `SetClassVar`
  (`1_core/proto/enscript.c:211`), file handles, `CopyFile`, `DeleteFile`, and RPC sends.
- **Provenance survives the app.** `extra.pack` on the function, a badge on the node drawn by the
  path that already draws a dependency badge off a node's own key
  (`src/canvas/nodeitem.cpp:373-390`), and one comment line above the stamped method in the `.c`.
- **No pack has a URL.** No update endpoint, no fetch, no version check. The moment a pack can update
  itself, the disclosure the user read describes a version that is no longer installed.

So: what stops a pack quietly writing `GetGame().RequestExit(0)` into a mod? It cannot write it as
text at all. If it builds it from nodes, the method appears in the disclosure list before the pack is
ever enabled, the stamped function carries the pack's name in the generated file, and the node on the
canvas wears the pack's badge. There is no path where the code arrives and nothing names its author.
Claiming more than that would be a lie: **nobody reads a 40-node disclosure list, and everyone clicks
Enable.** Its real value is after something has gone wrong and somebody is looking for the cause. It
is worth building for that case and worth being honest that it does not do the other one.

### The three checks that are actually checks

**Refusal of code-carrying refs**, above. One pass over `body.nodes`.

**Layout keys, on every node in every body.** `nodefmt::isValidValue` (`src/graph.h:221`) against
each `opts` key, at load and again at stamp.

**Name collision on the stamp.** A stamped function writes a method into the user's class.
`src/codegen.cpp:2260-2263` already builds `takenMethods` from every generated method and every
declared function, and `takenMembers` from every variable, and already refuses a collision with a
warning (`:2273-2289`). The stamp checks the same sets before creating the function and offers a
rename, so the collision is caught where there is somewhere to say why rather than at generate time.
Two packs both stamping `Refuel` is the ordinary case, not an exotic one.

**A Set Timer inside a stamped body works and needs one thing.** Codegen walks `graph.nodes` for
timing nodes (`src/codegen.cpp:2265`) without caring which function the chain belongs to, so a timer
inside a stamped function still gets its member and its callback. Two stamps of the same pack node
would derive the same name and collide, which DZ320 already reports
(`src/analysis.cpp:1861-1870`). The stamp seeds `opts["name"]` per stamp, which is what
`bi::timingName` already falls back to (`src/builtins.h:139-143`).

---

## Compatibility: the `tpl.*` migration path

Saved projects from the Electron build reference `tpl.*` keys, so the format is load bearing.

**The migration is an alias and nothing else.**

- The sixteen shipped templates become the built-in pack `dayz-essentials`, with `code` bodies.
- `tpl.<id>` stays a permanent alias for `pk.dayz-essentials.<id>`, resolved in **one** place.
- **The writer keeps writing `tpl.`** for those sixteen, so a project saved here still opens in the
  Electron build, which `DESIGN.md:8-11` requires in both directions.
- Nothing rewrites keys on load. No saved project is orphaned.

New pack nodes never reach the `.sdzn` as keys at all, because they stamp into a `fn.call`, so a
project using a pack opens in the Electron build as ordinary functions and ordinary calls. That is
the third payoff of stamping and it means this design costs the Electron compatibility promise
nothing.

**One resolver, called from three sites.** `libraryDefFor(key)` in a new `src/library.h/.cpp`,
called from `Document::defForKey` (`src/document.cpp:16`), codegen's `defOf`
(`src/codegen.cpp:444`) and the analyser's `resolveDef` (`src/analysis.cpp:632`). Those three are
already three copies of the same decision and they have already drifted, which is what produced the
`unknown node` bug. A test asserts the three agree on every key the built-in pack declares.

---

## Diagnostics this adds

The app's own findings take DZ4xx, clear of the existing DZ101 to DZ118 and DZ301 to DZ320. Pack
findings take `pk.<packId>.<ruleId>` and cannot look like a DZ code.

| Code | Severity | Fires when |
| --- | --- | --- |
| DZ401 | Info | A stamped function's pack is installed at a different version than the stamp records. Hint: Update from library shows what changed. |
| DZ402 | Warning | A stamped function's body no longer matches its `bodyHash`, so it was edited by hand. Hint: the edit is yours to keep, and Update from library would discard it. |
| DZ403 | Warning | Two stamped functions from different packs generate the same method name. Hint: rename one; the generator writes only the first (`src/codegen.cpp:2273-2281`). |
| DZ404 | Info | This script uses nodes from a pack that is not installed. Hint: the code still generates, but the pack's rules and documentation are not being applied. |
| DZ405 | Warning | A pack rule names a pin the node does not have. Unreachable through the installer; fires for a hand-edited `.sdznpack` or `.sdzn`. |

Nothing here is an Error, and that is deliberate. No diagnostic in this codebase stops generation
(codegen keeps its own separate warnings, `src/codegen.h:22-33`), and none of these five describes
a file that will not compile.

---

## The staged plan

Each stage ships on its own and is worth shipping on its own.

### Stage 0: one resolver, and fix the sixteen. Hours, not days.

- `src/library.h/.cpp` with `libraryDefFor(key)`. The sixteen templates become the built-in
  `dayz-essentials` pack. `tpl.<id>` aliased permanently, writer unchanged.
- Called from `Document::defForKey` (`src/document.cpp:16`), `defOf` (`src/codegen.cpp:444`) and
  `resolveDef` (`src/analysis.cpp:632`). A test that the three agree.
- Retype `arr.new` elem, `json.load` type and `json.save` type from `string` to `typename`. Move
  `obj.member` field, `obj.setMember` field, `misc.methodRef` name, `timer.callLater` fn and
  `timer.remove` fn from pins to `opts`, with the identifier shape check.
- The sixteen appear in the palette and the canvas add menu through `nodeIndex`
  (`src/nodeindex.h:76`), with `nodeSummary` and `nodeCautions` answering for their keys.

**Why this first.** It stops sixteen nodes drawing as `unknown node` with no pins, it makes them
placeable and searchable for the first time, it fixes eight of them generating code that does not
compile, and it is the only item in this document that moves the number the project is trying to
move. `engineering-notes.md` puts third-party method conversion at 23 percent against 64 percent on
the user's own code, and `src/templates.cpp:21-24` names the gap: string building, member access,
array iteration, file IO. That is what the sixteen are for. Everything else in this document is worth
nothing to the other 77 percent, because a method that fell back to Raw has no nodes to collapse.

### Stage 1: Collapse to node. About a week.

- The gesture, the boundary classifier, the four refusals with counts, the pure-node copy rule.
- A `GraphFunction` is created and the caret lands in the name field. The identifier check that
  already guards a custom event applies (`src/mainwindow.cpp:2188-2193`). The one-line note about
  Get/Is/Has naming.
- The call node is `fn.call`, which already draws, generates, analyses and round-trips. **Nothing in
  `codegen.cpp` changes.**
- Double-click frames the `fn.entry` node. `Expand`, refused above one caller.

This is a canvas gesture plus one graph rewrite against model and generator code that already exists.
It is the whole of the authoring request at zero authoring burden.

### Stage 2: rules on functions, before packs exist.

- The nine predicates, in one new pass in `analysis.cpp`, reading from a new field on
  `DependencyContext`.
- Rules attached to a `GraphFunction` through `extra.rules`, editable in the function's own panel.
  A user can put "this must run after the Server Only node" on their own helper without a pack in
  sight, and that is worth having on its own.
- The four rules about rules, enforced here rather than at pack install, so they are already true
  before a pack can exist.
- Preview each rule against the current graph as it is written, so a rule that fires nowhere is
  visible while it is being authored.

### Stage 3: the pack file.

- The format above, read only. `body` graphs for third-party packs, `code` snippets built in only.
- Install, enable, disable, list. The disclosure card. The refusal set.
- Stamp, with the collision check and the rename offer. `extra.pack` provenance. The comment line in
  the generated `.c`.
- The dependency-rule branch in `dependencyOf`, which brings DZ314, DZ315 and DZ316 along for free.
- `packtest` in `tests/`, modelled on `tests/coretest.cpp:417-452`, carrying a deliberately hostile
  pack: a `bi.raw` node, a `trivia.before` holding `GetGame().RequestExit(0);`, a rule naming a pin
  that does not exist, a node whose signature collides with a declared function, and a third-party
  pack with a `code` body. The assertion is the same one that file already makes: none of it reaches
  the generated script, and each refusal is reported.
- A publish gate that is the `importtest` bar applied to a pack: stamp every node into an empty
  graph, generate twice, and refuse to publish on any difference.

### Stage 4: living with packs.

- `Update from library`, with the signature diff and the hand-edit refusal.
- DZ401 to DZ405.
- The missing-pack banner, which says the code still generates and names what is missing.
- Authoring a pack from stamped functions in the current project, which is the path that turns
  collapse output into a pack without anybody writing JSON.

---

## What we are not building

Each of these was considered and rejected. The reason is here so it does not get relitigated in a
month.

**Macros, in the Unreal sense.** A macro inlines its body at each call site and would buy the
two-exec-output shape that collapse refuses, which is genuinely the most-wanted case. It costs the
round trip. Inlining is a new generator: splice a subgraph's statements into another chain, rename
its temporaries, and survive the same macro being reached twice. `Ctx` already carries `temps`,
`chainCache`, `pendingAfter` and `afterNode` to contain that class of problem for **one** case, a
Sequence driving the same tail twice (`src/codegen.cpp:101-147`, `:1308-1325`), and inlining
multiplies it. Rejecting this is right for this tool and it costs the feature its best case.

**Composite pack nodes: a pack node that declares class members and generates methods.** This is the
second request after this lands, and the answer is no. The Timer cost roughly 200 lines across three
modules and four dedicated diagnostics for one node. Letting strangers author that shape is letting
strangers author every collision the Timer had to be taught to avoid. The four timing nodes stay hard
coded, which means the app has two classes of node and only one is authorable. That is a real
inconsistency and it is cheaper than the alternative.

**Vendoring `.c` files from a pack into the user's mod.** It is the widest surface for the smallest
gain. It forces per-mod class name prefixing so two mods carrying the same pack do not compile two
classes of the same name, it forces an update story for files already written, and it makes every
pack an ecosystem-fragmentation event. `expansion.md` records hand-written versioning as the
single largest recurring tax in the largest DayZ mod there is, and this would collect the same tax.
A pack author who needs to ship code should ship a mod, and the pack can bind to it as a dependency.

**Snippet bodies for third-party packs.** A snippet is text, and every structural hazard in this
design is a hazard about text: holes inside string literals, brace depth going negative,
`EnforceScan` reporting only net balance (`src/enforce/lexer.h:66`). Refusing snippets deletes the
whole hazard class instead of checking it. The built-in pack keeps them because it is us, and because
`{a} + {b}` as a helper call would be worse code.

**Live linking: a project resolving pack nodes at generate time.** The generated `.c` would depend on
a file the mod folder does not carry, and a regenerate on a machine without the pack would produce a
mod calling a method nobody wrote. Stamping costs the ability to fix a bug once and have it reach
everybody, and that is the right trade for a tool whose output is what ships.

**Freezing pack definitions into the `.sdzn` (a `nodeDefs` block).** Considered as the alternative way
to make a project open without its pack. Stamping already achieves it with no format block, no
resolution precedence question, and no way for two people on one team to generate different code from
the same project.

**Blocking export when a pack is missing.** Considered, and it contradicts the repo's own established
failure mode: leave the node out, put the reason in the warnings, keep generating
(`src/codegen.cpp:920-925`). Under stamping the question does not arise at all, because the function
is in the graph.

**A pin `role` field for name and type pins.** `PinKind::Typename` already exists and already emits
unquoted (`src/pins.cpp:64`, `src/codegen.cpp:1598`), and identifiers belong in `opts` the way the
Timer's name already does (`src/builtins.h:139-143`). Adding a schema field to solve a problem the
type system already solves is a field we would carry forever.

**Any network: a registry, an update check, a signature, a `sourceUrl` that is fetched.** A pack
travels as a file the user opens. The moment a pack can update itself, the disclosure the user read
describes a version that is no longer installed. Signing is not in scope either: a hash proves two
files are the same file, and there is no key infrastructure here that would prove anything more.

**Pack-to-pack dependencies.** Install would need resolution and the missing-pack banner would need a
tree. A pack may require a mod addon, which is the dependency that actually matters in DayZ. A pack
author who wants to build on another pack copies the node.

**Importer recognition of pack nodes.** Reverse-matching generated code back to a pack node would be
ambiguous between two packs with the same skeleton, and it would make the same `.c` import
differently on two machines depending on which packs happen to be installed. An importer whose output
depends on local state is an importer nobody can test. Emitting a marker comment for the importer to
key on is rejected too: it puts tool noise into the user's mod and breaks byte-identity with the file
that was already there.

**Multiple data outputs from a collapsed node**, until `GraphParam` (`src/graph.h:112-115`) gains a
direction and `functionSignature`, `functionEntryDef`, `functionCallDef` and the argument builder
(`src/codegen.cpp:1774-1777`) follow it. Enforce expresses this with `out` parameters and vanilla
uses them constantly (`3_game/entities/entityai.c:2741`, `3_game/global/world.c:33`). This is
deferred, not rejected.

**A pack rule that can suppress a DZ finding.** The DZ rules are this tool's own claim about DayZ. A
file a stranger handed you must not be able to retract one in your project.

---

## What this design is bad at

Stated here rather than discovered later.

**Expand stops working the moment the feature is used properly.** It is refused above one caller,
which is correct, and it means the natural undo of a collapse breaks as soon as the user places their
new node a second time. They will read that as a bug.

**The one-exec-output limit refuses the most-wanted shape.** Half of what anyone wants to collapse is
a Branch with two useful outcomes, and the answer here is "return a bool and branch outside", which
is more nodes than they started with.

**A pack is a copy, so a bug never gets fixed by fixing the pack.** No way to find which of your own
mods carry the broken version except opening them one at a time.

**Pack rules are shallow and will look shallow next to the ones that ship.** DZ112 knows an `Object`
cannot be `new`ed because it walks the real inheritance graph (`src/analysis.cpp:1094-1104`). A pack
rule can say "must run after the Server Only node" and can say nothing that needs the catalogue's
knowledge of DayZ. Every request to extend the list is a request to add a case to `analysis.cpp`,
which is where the tier-three idea stops being data driven.

**Two people who collapse the same idea get two incompatible nodes with the same name.** Identity is
a `packId` somebody typed. A project carrying two authors' `PlaceholderCheck` shows two identical
palette rows and no way to tell them apart before placing one.

**It moves the conversion number by zero.** Custom nodes are made out of nodes that already
converted. This is worth nothing to the 77 percent of third-party methods that fall back to raw
Enforce (`engineering-notes.md`), which is why stage 0 is the sixteen templates and not this.
