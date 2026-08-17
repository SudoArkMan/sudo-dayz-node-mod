# The interface layer, and why it is not a UI designer

**None of this is built.** It is a design note for work that has been asked about, kept in the
repository because the measurements it rests on are real and because the shape of the design is the
argument. Nothing here is in the 0.1.0 release. File names and line numbers were true when this was
written rather than being kept current.

Every count below was taken from a working copy of four trees: vanilla `P:/gui`, the DayZ Expansion
experimental branch, Dabs Framework and Community Framework. Together they hold **482 `.layout`
files, 25 `.imageset` files, 209,691 lines of layout text and 12,972 widget entries** carrying 7,696
distinct names. No third-party source is reproduced here. What is quoted is a path, a line number and
a count.

---

## The one-paragraph version

The answer to "is a UI designer worth building" is no, and the reason is in the file. Sixty-two
percent of Expansion's layout property lines are geometry, which is what a designer is for, which
Workbench already does, and which has produced **zero** of the failures this corpus contains. Twelve
percent are names, and every silent failure found here is one of those names disagreeing with a name
in a `.c` file. So build the thing the tool is already good at: read the `.layout`, and turn the
names in it into pins, members and diagnostics. What the user gets is a script kind that knows its
own layout, 628 `FindAnyWidget` string lookups replaced by wires typed from the widget the string
named, and a check pass that finds about 180 real name mismatches in this corpus, none of which the
engine reports. What it costs is a second reader for a file format the tool did not own before, the
same round-trip obligation `importtest` already imposes on `.c`, and an honest boundary: nothing here
draws anything, so anybody who needs to move a widget still opens Workbench. That boundary is
acceptable because moving widgets is not what breaks.

---

## What the file is, measured

### The split that decides this

Expansion's 233 layouts hold 43,038 property lines. Sorted by what a property is for:

| What the property says | Lines | Share |
|---|---:|---:|
| Geometry and flow: `position`, `size`, the four `*exact*` flags, `halign`, `valign`, `Padding`, `Margin`, `Rows`, `Columns`, `Size To Content`, `priority`, `pivot`, `rotation` | 26,709 | 62 percent |
| Names and paths: `scriptclass`, `Binding_Name`, `Relay_Command`, `style`, `font`, `imageTexture`, `image0`, `text` | 5,230 | 12 percent |
| Appearance: `color`, `visible`, `ignorepointer`, text flags, blend modes | 11,099 | 25 percent |

A visual editor is a tool for the first row. It would have to reproduce the engine's layout solver
exactly: 106 distinct property keys, `Size To Content` in both axes, spacer flow, safe zones and
reference-resolution scaling, or the preview lies about what ships. A preview that lies is worth less
than no preview, because the user acts on it. And the first row is not where the bugs are. Every
finding in this document comes out of the second row.

### The grammar is three productions

```
member := block | prop
block  := [ IDENT [name] ] '{' member* '}'
prop   := KEY value* <end of line>
```

`.layout` always wraps a widget's children in an anonymous block. `.imageset` nests named blocks
directly. One reader covers both, and covering both is free: sprite names become checkable, which is
worth two findings on its own.

Thirty-one named block kinds appear across the four trees, plus the anonymous child block (6,030
occurrences) and `ScriptParamsClass` (837). The kinds are a closed set, and they are not the set the
naming suggests:

| Layout kind | Entries | Script type |
|---|---:|---|
| `PanelWidgetClass` | 2,551 | none, declared as `Widget` |
| `ImageWidgetClass` | 2,334 | `ImageWidget` |
| `TextWidgetClass` | 2,143 | `TextWidget` |
| `FrameWidgetClass` | 1,401 | none, declared as `Widget` |
| `GridSpacerWidgetClass` | 1,367 | `GridSpacerWidget` |
| `ButtonWidgetClass` | 1,062 | `ButtonWidget` |
| ... 25 more | | |
| `WindowWidgetClass`, `ContentWidgetClass`, `ThreeStateCheckboxWidgetClass`, `EmbededWidgetClass`, `ServerBrowserWidgetClass`, `SmartPanelWidgetClass` | 97 | none |

**Eight of the 31 kinds have no same-named script class, and they cover 4,049 of 12,972 widget
entries, 31 percent of the corpus.** There is no `PanelWidget` and no `FrameWidget` anywhere in
`P:/scripts`; the base is `class Widget: Managed` at `P:/scripts/1_core/proto/enwidgets.c:107`. A
mapping written as "strip `Class`" generates a member type that does not compile on a third of every
layout in DayZ. The table is data, not a rule.

### The traps in the text

- **Braces live inside strings.** 521 quoted strings across the corpus contain one, almost all the
  resource GUID prefix: `imageTexture "{0CABB20C60678953}DayZExpansion/Book/GUI/textures/paper_background.edds"`.
  A reader that counts braces before it tokenises strings mis-nests the tree. This is the same lesson
  `src/config/configtree.h:8-11` already records for `//` inside a quoted path.
- **Escaped quotes exist, twice.** `P:/gui/layouts/repro/alphamask.layout:71` and
  `P:/gui/layouts/script_console.layout`. Two sites in 35,553 strings, and both are vanilla.
- **The newline is significant.** A property runs to end of line and there is no terminator. This is
  the one structural difference from `config.cpp`, where `;` closes a value and newlines are trivia.
- **Nothing is a comment and nothing is included.** Zero `#include` lines and zero `//` lines in 468
  files. There is no preprocessor to model, and `EmbededWidgetClass` carrying
  `layout "{B10E12F621E0AB2F}Gui/layouts/examples/ButtonTemplate.layout"`
  (`P:/gui/layouts/examples/buttontemplatetest.layout:18`) is the only cross-file reference in the
  corpus: three sites, one file, in the examples folder.
- **The files are machine-written and it shows.** One space of indent per nesting level, exactly
  **one** blank line in 209,691, three lines with a tab in the indent, fourteen with trailing
  whitespace, and **467 of 468 files end without a trailing newline.** A writer that appends one
  rewrites the entire corpus on first save. That alone justifies building the reader the configtree
  way rather than as a model plus a pretty-printer.

---

## The reader

Built as `src/gui/layouttree.h/.cpp`, beside `src/config/configtree.h/.cpp` and shaped the same way,
because the same three decisions apply.

**Every construct remembers the text it was parsed from.** `ValueFormat` and `ClassFormat`
(`src/config/configtree.h:24-64`) exist so that opening a file and saving it gives back the same
bytes, and so one edit rewrites one line instead of reformatting somebody's mod. A layout construct
needs less: a lead span, a head span, a members list, a tail span and a footer span.

**The writer reuses that text unless the meaning changed.** `writeConfig` compares a signature
captured at parse time against one computed now (`src/config/configtree.cpp:523-529` for a value,
`:588` for a class header) and only regenerates what differs.

**Anything it cannot read is carried through, and the parse does not stop.**
`src/config/configtree.cpp:248-256` says it in as many words: text that is not understood lands in
the next member's lead and comes back out of the writer untouched, with a line recorded in
`ConfigFile::errors`. The importer refuses rather than guessing everywhere else in this repository,
and refusing here means refusing to *interpret*, not refusing to *open*.

That shape was tested against the whole corpus before this was written. A reference implementation of
the grammar above, with span capture and the reuse rule:

- **507 of 507 files parse and write back byte identical**, 482 layouts and 25 imagesets, across all
  four trees, with **zero** parse complaints.
- Renaming one widget and writing the file back changes **exactly one line, in all 482 layouts**.
- Against nine deliberately malformed inputs (unbalanced open brace, stray close brace at the top
  level, unknown block kind, file truncated mid-tree, property with no value, quoted key with spaces,
  CRLF throughout, tab indentation, UTF-8 BOM), the output is byte identical in all nine, and the
  three that are genuinely broken record a complaint naming the line.

That is the `importtest` bar applied to a second file format, and it is checkable the same way: a
`layouttest` that walks the search path, parses and writes every `.layout` and `.imageset`, and
refuses on any byte difference. Zero changed files, not negotiable.

What the reader refuses to do is guess. An unknown block kind is kept and reported, not mapped to
`Widget`. An unknown property key is kept and reported, not dropped. `colums` appears once in
Expansion where `Columns` was meant, and the right behaviour is to write it back unchanged and say so
once.

---

## The model

The tool generates one class per script. A `ScriptView` subclass is a class. So the layout attaches
to a **script**, not to a node, and the framework's own auto-wiring supplies the rest.

### A script that knows its layout

`ScriptEntry` (`src/project.h:13-28`) gains a layout path, carried in `extra` so the shape stays
compatible in both directions the way `DESIGN.md:8-11` requires. When it is set, three things follow
with no further authoring:

- codegen emits `override string GetLayoutFile()` and `override typename GetControllerType()`. In
  Expansion these two are effectively one decision: 129 classes override the first and 126 the
  second.
- the widget members are generated from the layout, not typed by hand.
- the Variable Manager lists them, and every one is placeable on the canvas.

The auto-wiring is what makes this safe, and it is worth reading before trusting it.
`ScriptView.LoadWidgetsAsVariables` (`DabsFramework/Scripts/3_Game/DabsFramework/MVC/ScriptView.c:238-266`)
walks the class's own properties, keeps the ones inheriting `Widget`, and calls
`root_widget.FindAnyWidget(property_name)` followed by `EnScript.SetClassVar`. A member whose name is
not a widget name in that layout is left null and nothing is said. That is the failure class, stated
by the framework itself in three lines.

Two details of that function constrain the model:

- `property_name_formatted.Replace(".", "")` at `:251` strips dots from the *field* name before the
  lookup. A script identifier cannot contain a dot, so a widget whose layout name contains one can
  never be auto-wired. It happens that no widget in 12,972 entries has a dot in its name, so this
  costs nothing today and would cost everything the day somebody uses the Dabs `Button.Icon`
  convention (`ScriptView.c:273-291`).
- `:255` and `:261` split on whether the field name equals the root widget's name, with the comment
  "fixes bug that breaks everything". The root is bindable, but only through the second branch. A
  generator has to know which branch its member will take.

### Four node kinds, and three of them already exist in another form

**`ui.widget`, a widget read.** Pure node, no exec pins, one output typed from the widget class
table. It generates nothing at the site: the member declaration comes from the layout. This is the
node that retires 628 `FindAnyWidget` calls, and the retirement is the point. A pin cannot be
misspelled, and `canConnect` (`src/graph.h:259-262`) refuses an `ImageWidget` wired where a
`ButtonWidget` is wanted before the mistake exists.

**`ui.bind`, a controller property.** The layout already names the full set: 472 `Binding_Name`
entries in Expansion over 268 distinct names, all inside a `ScriptParamsClass` block on the widget
they drive. The controller side is a member and a call: `NotifyPropertyChanged(name)`, 306 sites in
Expansion. One set node writes both. The type surface is small enough to model completely, because
`ViewController.GetControllerProperty` (`ViewController.c:230-248`) resolves a name to a `typename`
and hands it to `GetTypeConversion`, and the converters that exist are string, bool, int, float,
`Object`, `Vector`, `Widget`, `ScriptView` and `ObservableCollection`. Expansion declares 80
`ObservableCollection` members over 68 element types.

**`ui.command`, a relay handler.** 181 `Relay_Command` entries in Expansion over 87 distinct names.
The handler is a method with a fixed signature, which is what `GraphFunction` and `fn.entry` already
are (`src/graph.h:118-132`, `src/scriptapi.cpp:55-71`). Reading the relay names out of the layout and
offering each as an entry node is the whole feature.

**`ui.color` and `ui.hover`.** `ViewController` has no colour type, no visibility type and no image
type, so the theme is carried by hand: **1,129 `SetColor` calls and 1,004 `ARGB(...)` literals in
Expansion, which collapse to 157 distinct argument lists.** The top three account for 551 of them.
That is a palette resource, not a thousand literals. Hover is 96 `OnMouseEnter` and 95 `OnMouseLeave`
overrides totalling 3,431 lines, of which 164 set a colour, 72 switch on `w` and 75 compare `w ==`.
One node with a widget pin, a resting colour and a hover colour covers the shape, and the 27 that do
something else keep writing it by hand.

`ScriptParamsClass` carries 23 distinct property keys across the whole corpus. Five of them matter:
`Binding_Name` (551), `Relay_Command` (201), `Debug_Logging` (176), `Two_Way_Binding` (49) and
`Selected_Item` (6). Everything else is spacer and tab configuration on six vanilla script classes.
`Two_Way_Binding` is 6 sites in Expansion's 233 layouts, so it is a checkbox on the bind node and not
a concept.

---

## What the checks find, with counts

Run over Expansion against the four-tree search path. Every one of these is silent at runtime: no
engine error, no compile error, no log line outside `Debug_Logging`.

| Check | Checked | Findings |
|---|---:|---:|
| A widget member the framework will auto-wire, whose name is in no widget in the class's own layout | 747 | **30** |
| A `FindAnyWidget` string literal naming a widget that exists in no layout on the path | 614 | **7** |
| `Binding_Name` naming a property the controller does not declare | 432 | **37** |
| `Relay_Command` naming a method neither the view nor its controller has | 170 | **13** |
| `scriptclass` naming a class that exists in no `.c` on the path | 1,013 | **33** |
| `style` naming a style no `.styles` file declares | 1,535 | **3** |
| `set:X image:Y` naming a sprite the imageset does not carry | 195 | **2** |
| Two widgets with the same name in one layout, where `FindAnyWidget` takes the first | 12,972 | **33** |
| A layout no `GetLayoutFile`, `CreateWidgets` or any other string in any `.c` names | 233 | **28** |

About 180 findings in one mod. Some are known: the three dead fields on `ExpansionTeleporterMenu` are
`expansion.md` item 27, and this pass finds them plus 27 more of the same shape. Some are new. The
three unknown styles are all `Expansnion_04`, a transposition of `Expansion_04`, on three
`CheckBoxWidgetClass` widgets in Market, Market and Quests. The two unknown sprites are both
`large_circle_half` in `dayz_gui`, in the helicopter and plane HUDs. `scriptclass "Horizo"` at
`P:/gui/layouts/debug/day_z_hud_debug.layout:420` is a truncated `HorizontalSpacer`, and it is
Bohemia's. Twenty-five of the 33 orphaned `scriptclass` names are Community Online Tools forms whose
script classes have been deleted while their layouts stayed in the PBO, which is also where most of
the 28 orphaned layouts come from.

Two of the checks need care, and the care is the design:

**The `FindAnyWidget` check has two false-positive sources and both are avoidable.** The raw pass
returns 11 names, of which three are the left half of a concatenation
(`FindAnyWidget("Tab_" + expansionTabIndex)` at
`DayZExpansion/Core/Scripts/5_Mission/DayZExpansion_Core/GUI/Options/OptionsMenu.c:28`, twice more
elsewhere) and one sits inside a `//` comment
(`ExpansionMarketMenuColorHandler.c:93`). Seven are real. So the extractor has to strip comments and
has to refuse a literal that is an operand rather than the whole argument. Both are things
`scanEnforce` already does for Raw nodes (`DESIGN.md:48-53`).

**The base chain has to be walked or the numbers are wrong.** A first pass without it reported 928
dead widget members instead of 30, because `ScriptView` and `ScriptedViewBase` declare `Widget`
members of their own that no layout is expected to satisfy, and because local variables inside a
method are not members. This is the same defect `expansion.md` item 3 names in the lowering resolver:
`functionIn` at `src/enforce/lower.cpp:2608` never walks `graph.baseClass` up through the project.
One fix serves both.

---

## What this is bad at

**It draws nothing, and 62 percent of the file stays in Workbench.** Twenty-six thousand seven
hundred and nine geometry property lines in Expansion alone are outside this design entirely. A user
who wants a button two pixels left gets no help. The honest sentence is that this tool owns the
names and Workbench owns the boxes, and a user who expected a designer will read that as the feature
not being built.

**`Relay_Command` can never be checked to a hard answer.** `ViewBinding.InvokeCommand` at
`ViewBinding.c:222-224` bubbles to `context.GetParent()` when the call is not handled, and the parent
is a runtime tree that no static reader can see. `LoadRelayCommand` (`ViewController.c:273-311`)
compounds it: the name may be a `RelayCommand`-typed property on the controller, or a typename
resolved through `relay_command_name.ToType()` at `:303`, or a plain method, with the fallback logged
as "Assuming its a function on the ViewController / ScriptView" at `:193`. Of the 13 findings above,
`ExpansionTeleporterMenu`'s `OnHideHudButtonClick` exists only on `ExpansionQuestMenu` and is almost
certainly a copy-paste bug, while `ExpansionP2PMarketMenuCargoItem`'s `OnItemButtonClick` exists on a
plausible parent and is probably fine. The check is a warning forever, and a warning that is wrong
sometimes is a warning people learn to ignore.

**"Exists" is not "is the one you meant".** Of 614 `FindAnyWidget` literals, 423 name a widget that
appears in exactly one layout on the path, 180 name one that appears in more than one, and 11 name
one that appears in none. For those 180 the check can only say the name is not invented. Worse, the
engine has the same ambiguity: 33 pairs of same-named widgets exist inside single layout files, 32 of
them in four vanilla files including 29 in `P:/gui/layouts/scene_editor/day_z_scene_editor.layout`,
and `FindAnyWidget` returns the first in tree order with nothing to say it chose.

**Texture paths cannot be checked from a mod source tree at all.** Expansion's repository contains
**zero** `.edds` and **zero** `.paa` files against 473 image path properties in its layouts. The
binaries are build output. Only sprite names survive, because `.imageset` is text, which is why
covering it was worth doing and why the image check tops out at 195 of 473 references.

**Nearly half the `FindAnyWidget` calls sit where the tool cannot name the layout.** 317 of 614 are
inside a class that overrides `GetLayoutFile` or calls `CreateWidgets` with a literal; **297 are
not**. Those are the hand-loaded layouts behind 58 `CreateWidgets` calls, the three surviving
Community Online Tools forms extending `JMFormBase`, the 20 `ScriptedWidgetEventHandler` subclasses,
and every `modded class` reaching into a layout somebody else loaded.
`IngameHud.FindAnyWidget("BadgeNotifierDivider")` is
`expansion.md` item 18 exactly: the only way to recolour a vanilla widget, and null the day Bohemia
renames it. For all 297 the check degrades to "this name exists somewhere on the path", which caught
seven real errors and will never catch a wrong-layout error.

**It creates a second writer for a file Workbench also writes.** Reusing the parsed source text
contains that for edits to existing widgets, proved above at one changed line per rename. It does not
contain it for *new* widgets, where the tool has to invent an indentation, a property order and the
exact set of properties Workbench would have emitted, and it will get that wrong in a way that shows
up as a diff nobody asked for. This is the strongest argument for the first version writing nothing
at all.

**It is worth nothing to somebody who has not made a layout yet.** The whole design starts from a
`.layout` on disk. The first-time case, which is the case a UI designer would actually serve, is the
one case this does not touch.

**Two-way binding gets modelled for six sites.** 49 in the whole corpus, 43 of them inside Dabs
Framework's own test layouts, 6 in Expansion. Any concept spent on it is a concept spent badly, which
is why it is a checkbox, and a checkbox is still a thing to explain.

---

## The boundary, said plainly

You open Workbench to create a layout, to place anything, and to change any of the 26,709 geometry
properties. You never open it to name a widget, to wire a widget to a field, to add a binding, or to
find out why a field is null. The split is not arbitrary and it is not a compromise: the geometry
half produced none of the roughly 180 findings above, and the naming half produced all of them.

That is acceptable. Workbench is already installed on every machine that can build a PBO, it already
does the geometry correctly by definition because it is the same code the engine runs, and the thing
it does not do is tell you that `Complete` is not a widget in `expansion_teleporter_menu.layout` and
has not been for as long as the file has shipped.

---

## The smallest first version worth shipping

**Stage 0: the reader and the check pass. No nodes, and no writing.**

- `src/gui/layouttree.h/.cpp`, the grammar above, with span capture and the reuse rule. The parse
  never stops; unread text lands in the next member's lead; complaints go in an errors list the way
  `ConfigFile::errors` does.
- `tests/layouttest.cpp`, the `importtest` bar for this format: parse and write every `.layout` and
  `.imageset` on the search path, refuse on any byte difference. The number is zero changed files.
- The widget class table as data, all 31 kinds, with the eight that map to `Widget` marked.
- The nine checks, reported through the shape `ConfigFinding` already defines
  (`src/widgets/configeditor.h:59` onward): a level, a path into the file, a message and a hint. The
  base chain walk that `expansion.md` item 3 already needs, shared with the lowering resolver.
- Surfaced the way the config editor already surfaces its rules, which
  `src/widgets/configeditor.h:12-17` states the reason for: a `files[]` path that is not on disk
  means a script module never loads and nothing anywhere says so. A widget name that is not in the
  layout means a field is null and nothing anywhere says so. Same argument, second file.

**Why this first.** It is the only part of the design that can be held to a fixed bar before anything
depends on it. It is worth something on its own to somebody who never opens the node canvas, because
it finds real bugs in a mod that already shipped. And it is the part every later stage sits on: no
pin can be typed from a widget until something reads the widget.

**What it deliberately leaves out.**

- **Every node.** No `ui.widget`, no `ui.bind`, no `ui.command`, no colour, no hover. Those come
  after the reader is proved, and they come in that order because that is the order of the counts
  behind them.
- **Writing a `.layout`, at all.** Stage 0 opens files and never saves one. That removes the
  second-writer hazard from the first release completely, and it means the round trip is proved by a
  test rather than by users.
- **The palette resource.** 157 distinct ARGB values is a good number and it is still a new file
  format, a new panel and a migration story for 1,004 existing literals. Not first.
- **Anything about geometry.** No preview, no positioning, no size solver, no picture of the widget
  tree beyond a plain tree of names and kinds.
- **Any attempt to guess a layout for the 297 `FindAnyWidget` literals whose class does not name one.**
  They get the weak corpus-wide existence check and a note saying which check they got. Guessing
  which layout a `modded class IngameHud` is reaching into is the kind of inference this repository
  refuses everywhere else, and it should refuse it here.
