# The interface layer: the decision

The question was whether this tool should grow a UI designer. The answer is no, and it is not a
close call. Build the `.layout` reader and the name-checking pass first, put the binding nodes on
top of it, and do not draw anything.

Two designs were written for this question, one arguing for a layout editor and one arguing for a
binding layer with no picture in it. Both were checked line by line against the four trees on this
machine. This file is the judgement, the ordering, and the reader design that both approaches need
either way. The long-form binding design is `docs/notes/interface-layer.md`; the corrections at the
end of section 2 apply to it.

Counts here were taken today from vanilla under `P:/`, the DayZ Expansion experimental branch, Dabs
Framework and Community Framework. Where a number differs from one already in
`docs/notes/expansion.md`, the number here was recounted and the difference is called out.

---

## 1. The answer, in one paragraph

Sixty-three percent of Expansion's layout property writes are geometry. That is what a designer is
for, that is what Workbench already does correctly by construction because it runs the engine's own
layout code, and it produced none of the failures in this corpus. Eleven percent are names, and
every silent failure found here is a name in a `.layout` disagreeing with a name in a `.c`. A layout
editor competes with a shipped tool on the half of the file that does not break. A reader plus a
check pass owns the half that does, and nothing else on a modder's machine looks at both files at
once. On top of that, the tool's claim is that it removes code: a layout editor removes none, and
the binding layer removes 628 `FindAnyWidget` string lookups, 1,129 `SetColor` calls and 191 hover
overrides in one mod. Both proposals reached the same conclusion about where the value is. Only one
of them acted on it in the first release.

---

## 2. What was measured

A span-preserving reader and writer for the grammar in section 4 were written and run over
everything on disk.

| | count |
|---|---:|
| `.layout` files | 482 |
| `.imageset` files | 26 |
| lines of layout text | 209,691 |
| widget entries | 12,972 |
| `ScriptParamsClass` blocks | 837 |
| distinct widget kinds | 31 |
| distinct property keys on widgets | 114 |
| distinct `ScriptParamsClass` keys | 23 |
| property writes | 168,924 on widgets, 1,088 in script params |
| maximum nesting depth | 11 |
| largest file | `P:/gui/layouts/xbox/controls_screen.layout`, 602,245 bytes, 1,172 widget headers |

Round trip, run today:

- **508 of 508 files parse and write back byte identical**, with **zero parse complaints**.
- Renaming one widget and writing the file back changes **exactly one line, in all 482 layouts**.
- Nine deliberately malformed inputs (unbalanced open brace, stray close brace, unknown widget kind,
  file truncated mid-tree, property with no value, quoted key with spaces, CRLF throughout, tab
  indentation, UTF-8 BOM) all come back byte identical, and the three that are genuinely broken
  record a complaint naming the line.

Where the two proposals agreed with each other and with this recount: 482 layouts, 12,972 widget
entries, 31 widget kinds, 209,691 lines, 168,924 property writes, 105 style names across the three
`.styles` files, 57 fonts over 2,894 references, 496 GUID-prefixed image paths over 133 distinct
values, 198 distinct `set:` sprite names, 1,576 `scriptclass` sites, and Expansion's 472
`Binding_Name`, 181 `Relay_Command` and 6 `Two_Way_Binding`.

### Corrections that both proposals need

- **508 files, not 507.** Both search paths stopped at `P:/gui` and missed
  `P:/graphics/textures/postprocess/vignetteframes.imageset`, which the project file names at
  `D:/SteamLibrary/steamapps/common/DayZ Tools/Bin/Workbench/dayz.gproj:25`. The search path has to
  come from the `.gproj`, not from a guess about where files live. That is the whole lesson of the
  miss, and it is worth more than the one file.
- **26 imagesets, not 25**, for the same reason.
- `docs/notes/interface-layer.md` says 468 files in two places where the number is 482. The ratios
  it draws off that denominator (one blank line in the corpus, one file ending with a newline) are
  right; the denominator is not.
- `ConfigFinding` is at `src/widgets/configeditor.h:61`, not `:59`. `functionIn` is at
  `src/enforce/lower.cpp:2609`, not `:2608`.

### Corrections to the layout-editor proposal, which decide the judgement

- **Eight of the 31 widget kinds have no same-named script class, and they cover 4,049 of 12,972
  entries, 31.2 percent of the corpus**, not five kinds covering 35 entries. There is no
  `PanelWidget` (2,551 uses) and no `FrameWidget` (1,401) anywhere in `P:/scripts`; the base is
  `class Widget: Managed` at `P:/scripts/1_core/proto/enwidgets.c:107`. The full list is
  `PanelWidgetClass`, `FrameWidgetClass`, `WindowWidgetClass` (62), `ContentWidgetClass` (20),
  `ThreeStateCheckboxWidgetClass` (10), `EmbededWidgetClass` (3), `ServerBrowserWidgetClass` (1),
  `SmartPanelWidgetClass` (1). A generator that strips `Class` from the layout kind emits a member
  type that does not compile on a third of every layout in DayZ. This is the single most
  consequential fact either proposal got wrong or right, and only one got it right.
- **28 Expansion layouts are named by no string in any Expansion `.c`, not 40.** The 40 was inherited
  from `docs/notes/expansion.md:19`; the recount gives 28, which matches the binding proposal.
  Likewise `ARGB(` is **1,004** sites in Expansion, not the 984 at `expansion.md:356`.
- **33 names are duplicated inside a single layout, across five files, not one.** The
  layout-editor proposal checked direct siblings and found the one pair, `top` at
  `P:/gui/layouts/xbox/day_z_ingamemenu_xbox.layout:219` and `:630`, both under `play_panel_root` at
  `:42`. `FindAnyWidget` searches the whole subtree, so the file-wide count is the one that matches
  what the engine does.
- **No Dabs layout contains a tab.** The one tab-indented file in the corpus is vanilla's
  `controls_screen.layout`. The related observation that indentation drifts inside a single file is
  correct and worth keeping: `DabsFramework/GUI/layouts/sample_mvc.layout:30-40` runs at five spaces
  and drops to four for two lines.
- The drag-ability arithmetic does not close: 4,691 plus 910 plus 6,889 is 12,490 against 12,972.
  Counting direct children of a `GridSpacerWidgetClass`, `WrapSpacerWidgetClass`,
  `SpacerWidgetClass` or `ScrollWidgetClass`, which is what the engine positions, the split is
  **3,032 spacer-managed (23.4 percent), 1,820 more sized by their own content (14.0 percent),
  8,120 free (62.6 percent)**. The conclusion survives in weaker form: a third of the widgets in
  this corpus ignore the number a drag would write.
- 94 widgets carry two brace blocks, not 222. The observation itself is right and the reader has to
  handle it: `P:/gui/layouts/day_z_hud.layout:286-291` closes the children block and opens a second
  one holding `ScriptParamsClass`.

### Three findings from this recount that neither proposal has

- **The imageset index is keyed on the declared `Name`, not the filename.** Four of the 26 imagesets
  differ: `console_toolbar.imageset` declares `toolbar`, `dayz_additional_gui.imageset` declares
  `dayz_additional_gui_unique`, `expansion_book_assets.imageset` declares `book_assets`,
  `expansion_gui.imageset` declares `expansion_gui_logos` (`P:/gui/imagesets/ccgui_enforce.imageset:2`
  is the canonical shape). Keying on the filename produces 66 findings that are all wrong. Keyed on
  the declared name, **1,691 of 1,693 sprite references resolve** and the two that do not are both
  `dayz_gui large_circle_half`.
- **`halign top_ref` appears 11 times**, all at line 15 of eleven Expansion Community Online Tools
  form layouts, starting with
  `DayZExpansion/GUI/layouts/COT/General_Menu.layout:15`. A vertical alignment token in a horizontal
  field, copied eleven times. It is the only enumerated value outside its domain in 170,012 property
  writes, which is exactly why a domain check is cheap to run and finds the thing nobody looks for.
- **The `.gproj` is the index root.** `dayz.gproj:14` lists the imagesets and `:28` lists the styles
  files. The catalogue does not need a hardcoded list of either, and a mod's own gproj extends both.
  This came out of the layout-editor proposal and it is the best idea in it.

---

## 3. The five questions

**What does a modder get that they do not have today?** Workbench draws the boxes and it is right by
definition. Neither proposal claims to beat it. What nothing on the machine does is hold the
`.layout` and the `.c` open at once and compare the names, and that is where every failure in this
corpus lives. A tree with a property panel is a worse Workbench for the geometry and no better for
the names. The check pass is the answer to this question and it does not need a canvas.

**What does it cost to build and keep working?** The layout-editor proposal prices its first version
at about 3,900 lines and its canvas at 2,800 more, and it is honest that the canvas needs an anchor
and spacer solver reverse engineered from 12,972 examples, `.edds` decoding through LZ4 linked
blocks and BC3 and BC7, and a font story that covers 81 percent of references through two `.ttf`
files on the work drive and approximates the rest. Every one of those is a surface that rots on a
DayZ release, and `docs/notes/expansion.md:470` is the standing example of what a rotted fix looks
like: a shipped Expansion override targeting a vanilla field the engine no longer populates, with
nothing anywhere saying so. The reader and the checks read their vocabulary from data at load time,
so a DayZ update changes the data and not the code.

**Does the round trip survive?** Yes, and this is the weakest of the five questions, because
byte-identity on unmodified input is true by construction of a span-preserving reader. It proves the
tool is not destructive. It says nothing about whether a picture drawn from the tree is right. The
layout-editor proposal says this plainly and deserves the credit for saying it. The real writer risk
is not editing an existing widget, which measures at one changed line in all 482 layouts, but adding
one: a new widget needs an invented indentation, property order and default set, and it will differ
from what Workbench emits in ways nobody asked for. The first release should not write at all.

**Does it fit what the tool is?** The tool generates one class per script, and `ScriptEntry`
(`src/project.h:13-28`) is where a layout path belongs, carried in `extra` so the `.sdzn` stays
compatible in both directions the way `DESIGN.md:9-10` requires. A layout attaches to a class, not
to a node. The framework then does the rest by itself:
`DabsFramework/Scripts/3_Game/DabsFramework/MVC/ScriptView.c:238-264` walks the class's own
properties, keeps the ones inheriting `Widget`, and calls `FindAnyWidget(property_name)` followed by
`EnScript.SetClassVar`. A member whose name is not a widget name is left null and nothing is said.
That is the failure class, stated by the framework itself in three lines, and it is the shape the
tool is already built to fix.

**What breaks if it is wrong?** A bad graph shows a diagnostic. A bad layout ships a broken menu.
The layout-editor proposal's own risk register names the solver as its largest schedule risk and
says the only way to retire it is repeated screenshot diffing against the diag client. A tool that
draws a menu slightly wrong is worse than a tool that draws nothing, because the user acts on the
picture. Stage 0 below cannot break anything, because it does not write.

---

## 4. The `.layout` reader

Both approaches need this and it is the same in both. It goes in `src/gui/layouttree.h/.cpp`, beside
`src/config/configtree.h/.cpp` and shaped the same way. Not `src/layout/`:
`src/enforce/layout.h` is already the node auto-layout module and the name is taken.

### The grammar is line-oriented

A property runs to end of line and there is no terminator. That is the one structural difference
from `config.cpp`, where `;` closes a value and newlines are trivia, and it is the difference a
token-stream reader gets wrong. Five line kinds cover everything:

```
blank or unrecognised   ->  kept verbatim
}                       ->  close
{                       ->  open an anonymous children block
CLASS [NAME] {          ->  open a widget
KEY value...            ->  a property, values run to end of line
```

`P:/gui/layouts/day_z_logout_dialog.layout:1-16` is the canonical shape: header on one line,
properties, then a lone `{` opening the children.

Four traps, all measured:

- **Braces live inside quoted strings.** The resource GUID prefix puts one in every image path:
  `imageTexture "{0CABB20C60678953}DayZExpansion/Book/GUI/textures/paper_background.edds"`. Tokenise
  strings before counting braces or the tree mis-nests. `src/config/configtree.h:8-10` already
  records the same lesson for `//` inside a quoted path.
- **A widget can carry two brace blocks**, children and then `ScriptParamsClass`
  (`P:/gui/layouts/day_z_hud.layout:286-291`, 94 widgets).
- **`ScriptParamsClass` has no name**, so the header rule has to accept `CLASS {` as well as
  `CLASS NAME {`.
- **A key can be quoted and contain spaces** (`"exact text" 1`, `"Size To Content V" 1`), so a
  quoted string is one token before the line splits on whitespace.

There are zero `#include` lines and zero `//` lines in the corpus. There is no preprocessor to model.
The only cross-file reference is `EmbededWidgetClass` carrying a `layout` path, three sites in one
file under `P:/gui/layouts/examples/`.

### The three decisions, taken from configtree

**Every construct remembers the text it was parsed from.** `ValueFormat` and `ClassFormat`
(`src/config/configtree.h:27-64`) exist so that opening a file and saving it gives back the same
bytes and one edit rewrites one line. A layout line needs less: the raw text, a kind, and a
signature captured at parse time.

**The writer reuses that text unless the meaning changed.** `writeValue` compares the parse-time
signature against one computed now (`src/config/configtree.cpp:523` for the head, `:529` for the
body) and regenerates only what differs. The signature functions themselves are at
`src/config/configtree.cpp:75-90`.

That discipline is not decoration here. `docs/notes/expansion.md:464` records
`keybinding_option.layout` at 166 lines against vanilla's 169, where the entire real difference is
two deleted `ignorepointer 1` lines, three `clipchildren` flips and two colours. Layouts have no
inheritance and no partial override, so the working practice is copy a vanilla file and change four
lines. A normalising writer turns that diff into 166 lines and the next merge into a manual one.
The corpus punishes it specifically: 481 files are LF and one is CRLF, exactly one of 482 ends with
a newline, and one file indents with tabs.

**Anything it cannot read is carried through and the parse does not stop.**
`src/config/configtree.cpp:248-255` says it outright: text that is not understood lands in the next
member's lead and comes back out of the writer untouched, with a line recorded in
`ConfigFile::errors`. The importer refuses rather than guessing everywhere in this repository, and
refusing here means refusing to interpret, not refusing to open. An unknown widget kind is kept and
reported, never mapped to `Widget`. An unknown property key is kept and reported, never dropped.
`colums` appears once in Expansion where `Columns` was meant, and the right behaviour is to write it
back unchanged and say so once.

### One reader, two file types, one boundary

`.imageset` is the same grammar: `ImageSetClass { Name ... RefSize ... Textures { ... } Images {
ImageSetDefClass <name> { Pos, Size, Flags } } }`. Covering it is nearly free and it buys the sprite
check. `.styles` is standard XML and gets `QXmlStreamReader`, not this reader. `.meta` uses a
`class X : Y {` inheritance form that no layout uses and nothing in this feature needs it.

### The catalogue, read from data

- 31 widget kinds with their legal property keys, and the eight that resolve to `Widget` marked. The
  table is data, not a rule about stripping `Class`.
- Value domains, which are almost all enumerated: `mode` is blend, additive or opaque; `fixaspect`
  is fixwidth, inside, outside or none; `halign` and `valign` take the six `_ref` and non-`_ref`
  forms; `color` and `text color` are always four floats; `position` and `size` are always two.
- The style index, from every `.styles` named by the project's `.gproj:28`. 105 names today.
- The imageset index, from every `.imageset` named by `.gproj:14`, **keyed on each file's declared
  `Name`**. 26 sets, 15,519 sprites today.
- The `reference` field index, harvested the way the script API index already is. A
  `ScriptParamsClass` key is legal if and only if the class named in `scriptclass` declares a field
  with the `reference` qualifier: `P:/scripts/3_game/gui/containers/sizetochild.c:2-6` declares the
  five keys the corpus uses on `SizeToChild` widgets,
  `P:/scripts/3_game/gui/effects/rotator.c:4` declares the `speed` used five times, and
  `DabsFramework/Scripts/3_Game/DabsFramework/MVC/ViewBinding.c:4-13` declares `Binding_Name`,
  `Selected_Item`, `Two_Way_Binding` and `Relay_Command`. The whole surface is 42 `reference`
  declarations in `P:/scripts` (19 float, 12 int, 7 bool, 4 string) plus seven in Dabs, against 23
  distinct keys used across 482 layouts. The script-params property sheet is generated from the
  class, and a key that is not a `reference` field is an error the tool can name.

### The refusal moves from the file to the pixel

This framing came out of the layout-editor proposal and it should survive even though nothing is
being drawn. The reader is lossless on the whole corpus, so opening a file it does not fully
understand cannot corrupt it. What a tool must never do is draw something it cannot justify, because
a modder who sees a plausible button will ship it. Open fully, interpret partially, and be loud
about which is which. When there is no canvas, "loud" means the check list.

---

## 5. The check pass

Three name-matched contracts bind a layout to a class and the Enforce compiler checks none of them
(`docs/notes/expansion.md:354`). The tool holds both halves at once. Run over Expansion against the
four-tree search path, every one of these is silent at runtime: no engine error, no compile error,
no log line.

| Check | Checked | Findings |
|---|---:|---:|
| A widget member the framework will auto-wire, whose name is in no widget in the class's own layout | 762 | **30** |
| A `FindAnyWidget` string literal naming a widget that exists in no layout on the path | 605 | **7** |
| `Binding_Name` naming a property the controller does not declare | 432 | 37 |
| `Relay_Command` naming a method neither the view nor its controller has | 170 | 13 |
| `scriptclass` naming a class that exists in no `.c` on the path | 1,013 | 33 |
| `style` naming a style no `.styles` file declares | 1,535 | **3** |
| `set:X image:Y` naming a sprite the imageset does not carry | 1,693 | **2** |
| Two widgets sharing a name in one layout, where `FindAnyWidget` takes the first | 12,972 | **33** |
| An enumerated property whose value is outside its domain | 170,012 | **11** |
| A layout that no string in any `.c` names | 233 | **28** |

Bold rows were reproduced independently for this document. The four unbolded rows come from the
binding proposal and are consistent with everything around them, but were not re-run here; treat
their exact figures as provisional until the check ships.

The 30 dead auto-wire fields are the same finding at two denominators, and the difference is the
design. Counting only fields a class declares itself gives 701 checked and 20 dead. Counting fields
up the base chain, stopping at the framework's own bases, gives 762 checked and 30 dead. The second
is the right one, because `PropertyTypeHashMap(context.Type())` at `ScriptView.c:244` enumerates
inherited properties too. It also needs the base-chain walk that `docs/notes/expansion.md:492`
already asks for in the lowering resolver, where `functionIn` at `src/enforce/lower.cpp:2609`
iterates one script's own functions and never walks `graph.baseClass` up through the project. One
fix serves both, which is the strongest argument for doing this work at all.

The findings are real and some are already known. `ExpansionTeleporterMenu`'s `Complete`,
`CompleteLable` and `CompleteBackground` are `docs/notes/expansion.md:474`; this pass finds them plus
27 more of the same shape, including seven on `ExpansionMarketMenu`, three on
`ExpansionBookMenuTabCrafting` and three on `ExpansionBookMenuTabServerInfoSettingCategory`. The
three unknown styles are all `Expansnion_04`, a transposition of `Expansion_04`, in
`DayZExpansion/Market/GUI/layouts/market/expansion_market_menu_chechbox.layout:19` and two others.
The two unknown sprites are both `large_circle_half`. Twenty-nine of the 33 same-name pairs are in
`P:/gui/layouts/scene_editor/day_z_scene_editor.layout`, which is Bohemia's.

Two of the checks need care and the care is the design:

**The `FindAnyWidget` extractor has to strip comments and refuse operands.** The raw pass returns 11
names; three are the left half of a concatenation and one sits inside a `//` comment. Seven are
real. `scanEnforce` already does both jobs for Raw nodes (`DESIGN.md:50-53`).

**`Relay_Command` can never be checked to a hard answer.** `ViewBinding.InvokeCommand` bubbles to
`context.GetParent()` at
`DabsFramework/Scripts/3_Game/DabsFramework/MVC/ViewBinding.c:222-224`, and the parent is a runtime
tree no static reader sees. `LoadRelayCommand` compounds it: the name may be a `RelayCommand`-typed
property on the controller, or a typename resolved through `relay_command_name.ToType()` at
`DabsFramework/Scripts/3_Game/DabsFramework/MVC/ViewController.c:303`, or a plain method, with the
fallback logged as an assumption at `:193`. That check is a warning forever and should be labelled
as one.

---

## 6. The order

**Stage 0. The reader and the checks. No nodes, and no writing.**

- `src/gui/layouttree.h/.cpp`: the grammar in section 4, span capture, the reuse rule, an errors
  list, the parse never stopping.
- `tests/layouttest.cpp`: the `importtest` bar applied to a second format. Parse and write every
  `.layout` and `.imageset` on the search path, refuse on any byte difference. The number to publish
  is **508 of 508 changed files: zero**, and the search path comes from the `.gproj`.
- The widget kind table as data, all 31, with the eight that resolve to `Widget` marked.
- The catalogue indexes from `.gproj`, imagesets keyed on declared `Name`.
- The checks, reported through the shape `ConfigFinding` already defines
  (`src/widgets/configeditor.h:61`), surfaced the way the config editor already surfaces its rules.
  The reason is the one written at `src/widgets/configeditor.h:13-17`: a `files[]` path that is not
  on disk means a script module never loads and nothing anywhere says so. A widget name that is not
  in the layout means a field is null and nothing anywhere says so. Same argument, second file.

Stage 0 is the only part of this that can be held to a fixed bar before anything depends on it, it
is worth something to a modder who never opens the node canvas, and every later stage sits on it: no
pin can be typed from a widget until something reads the widget.

**Stage 1. The script knows its layout.** `ScriptEntry` gains a layout path in `extra`. Codegen
emits `GetLayoutFile()` and `GetControllerType()`, which in Expansion are effectively one decision:
129 classes override the first and 126 the second. The widget members are generated from the layout
rather than typed by hand, with the type taken from the kind table and not from stripping `Class`.
The Variable Manager lists them.

Two details of `ScriptView.LoadWidgetsAsVariables` constrain the generator and both are in the
source. `property_name_formatted.Replace(".", "")` at `ScriptView.c:251` strips dots from the field
name before the lookup, so a widget whose name contains a dot can never be auto-wired; no widget in
12,972 entries has one today, and the day somebody uses the Dabs `Button.Icon` convention
(`ScriptView.c:273-291`) it costs everything. And `:255` and `:261` split on whether the field name
equals the root widget's name, with the comment "fixes bug that breaks everything" above it. The
root is bindable, through the second branch only.

**Stage 2. The nodes, in the order of the counts behind them.** `ui.widget`, a pure node typed from
the widget kind, which is what retires 628 `FindAnyWidget` calls and makes `canConnect`
(`src/graph.h:262`) refuse a wrong-typed wire before the mistake exists. Then `ui.bind`, one node
writing both the controller member and the `NotifyPropertyChanged` call, over 472 `Binding_Name`
entries. Then `ui.command`, a fixed-signature handler over 181 `Relay_Command` entries, which is
what `GraphFunction` (`src/graph.h:118-132`) already is. Then colour and hover, because
`ViewController` has no colour type, no visibility type and no image type, which is why Expansion
carries the theme by hand in 1,129 `SetColor` calls and 1,004 `ARGB` literals collapsing to a small
number of distinct argument lists, and why 96 `OnMouseEnter` and 95 `OnMouseLeave` overrides are
nearly all a `switch (w)` swapping two values.

`Two_Way_Binding` is 6 sites in Expansion's 233 layouts and 49 in the whole corpus, 43 of them inside
Dabs' own test layouts. It is a checkbox on the bind node, not a concept.

---

## 7. What would have to change for the answer to flip

Three things, and only the first is likely.

**The tool starts creating layouts instead of only reading them.** A template that emits a
`ScriptView` subclass and its `.layout` together has no Workbench file to open, and at that point a
tree with a property panel is the cheapest way to adjust what was generated. This is the real flip
condition, and it arrives from the template work rather than from anything in this document. Even
then it is a tree and a property panel, not a canvas.

**Somebody demonstrates the solver.** Before any pixel is drawn, the anchor and spacer solver has to
agree with the engine on a screenshot diff against the diag client, repeatedly, across the
`GridSpacerWidgetClass` interactions (`Columns`, `Rows`, `Padding`, `Margin`, `content_halign`,
`content_valign`, both `Size To Content` axes, `Ignore invisible`) plus `scaled`, `keepsafezone` and
`fixaspect`. If that demonstration exists, the canvas becomes a schedule question rather than a
research question. Until it exists, the canvas is not costed.

**The check pass finds nothing outside Expansion.** Run the checks over the mods that ship a
`.layout` in the installed corpus, which this project's PBO reader already opens at 9,343,981 entries
with zero refusals (`docs/notes/engineering-notes.md:108-113`). If dead auto-wire fields turn up at
anything like the 30 in 762 rate they do in Expansion, stage 0 has paid for itself and stages 1 and 2
follow. If they do not, the naming problem is Expansion-specific, the check is worth less than
claimed, and the honest response is to stop rather than to reach for the canvas as a consolation.

---

## What we are not building

**A visual layout canvas.** Workbench runs the engine's own layout code against the engine's own
atlases and is right by construction. This would reimplement the solver from 12,972 observed
examples and disagree somewhere, and the failure mode is a modder shipping a menu that looked right
here. Sixty-three percent of the property writes it would serve produced none of the findings in
section 5.

**An anchor and spacer solver.** Only the canvas needs it, it is the largest single risk either
proposal names, and it cannot be retired by reading files.

**`.edds` decoding, the nine-slice draw, and font atlases.** Nothing renders, so nothing needs
decoding. The container notes in the layout-editor proposal are worth keeping on the shelf: the LZ4
mip chain needs the linked-block dictionary form, the atlases need BC3 and BC7, and 18.7 percent of
font references have no `.ttf` on the work drive and would be approximated forever. That is a real
cost for a picture we are not drawing.

**Writing a `.layout` at all, in the first release.** Editing an existing widget is proved at one
changed line in all 482 layouts. Adding a new one is not proved at anything, because the tool would
have to invent the indentation, the property order and the exact default set Workbench emits. Not
writing removes the second-writer hazard from the release completely.

**Creating a layout from nothing.** The corpus practice is copy a vanilla file and change four lines
(`docs/notes/expansion.md:464`), and the byte-preserving reader already makes that safe. Revisit only
under the first flip condition in section 7.

**Any duplicate-with-a-tweak command.** Nine Expansion layouts exist only because a number could not
be set at runtime: five chat entry files of 148 lines each, byte identical except a font size
(`docs/notes/expansion.md:358`). The fix is `TextWidget.SetTextExactSize(int)`, which Expansion calls
twice in 337,448 lines. A command that makes producing that family easier multiplies the problem it
should be reporting.

**A texture browser or a GUID database.** All 496 direct image references carry the `{GUID}path`
form and the path component is right there, so nothing needs resolving to find a file. The GUID is
written back untouched because the engine resolves by GUID and the path is the fallback. Beyond that,
Expansion's repository contains **zero `.edds` and zero `.paa`**: the binaries are build output, so a
mod source tree cannot be checked for them at all. Only sprite names survive, which is why
`.imageset` was worth covering.

**`.meta` files.** A different grammar, no layout uses it, nothing here needs it.

**A palette resource, first.** The distinct `ARGB` argument lists in Expansion collapse to a small
set and that is a good number, but it is still a new file format, a new panel and a migration story
for 1,004 existing literals. It comes after the colour node, not before it.

**Two-way binding as a concept.** Six sites in Expansion's 233 layouts. A checkbox.

**Any guess about which layout a `modded class` reaches into.** Roughly half the `FindAnyWidget`
literals sit in a class that names no layout: the hand-loaded `CreateWidgets` cases, the surviving
Community Online Tools forms, the `ScriptedWidgetEventHandler` subclasses, and every `modded class`
reaching into a layout somebody else loaded.
`IngameHud.FindAnyWidget("BadgeNotifierDivider")` is `docs/notes/expansion.md:465` exactly: the only
way to recolour a vanilla widget, and null the day Bohemia renames it. Those get the weak
corpus-wide existence check, labelled as the weak one. Guessing the layout is the kind of inference
this repository refuses everywhere else.
