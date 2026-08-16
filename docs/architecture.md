# Architecture

How the parts fit together, for somebody who wants to change one. About 51,000
lines of C++17 across `main.cpp` and `src/`, linking Qt Core, Gui and Widgets
and nothing else.

[DESIGN.md](../DESIGN.md) is the interface contract: dock layout, theme, node
visual spec, wire geometry, interaction table. It is binding and it is not
repeated here. [CONTRIBUTING.md](../CONTRIBUTING.md) has the build, the test
suites and the round trip bar a change has to meet.

## The shape of it

```
catalog.json  -->  Catalog  -->  NodeDef  -->  canvas
                     |            ^
                     |            |
    .c file  -->  lexer  -->  parser  -->  lower  -->  Graph  -->  codegen  -->  .c file
                                                         |
                                                      analysis
```

Two directions through one model. A graph comes either from a project file or
from reading somebody's script, and it leaves as Enforce Script. The generator
is the inverse of the importer, which is what makes correctness measurable
instead of arguable: read a file in, write one out, compare the bytes.

## The layers, and why they are separated

`CMakeLists.txt` compiles the shared sources once each through four object
libraries. The split is load bearing rather than tidiness, and the file says so
at length. Short version:

| Tier | Links | Holds |
| --- | --- | --- |
| `nodemod_core` | Qt Core | lexer, parser, config tree, PBO reader, work drive, mod template |
| `nodemod_model` | adds Qt Gui | pins, graph, catalogue, project, builtins, codegen, analysis, events, node index, document |
| `nodemod_themed` | adds Qt Widgets | theme, mod deps, mod library, test run, lowering, layout, import, highlighter |
| `nodemod_panels` | Qt Widgets | the widgets and panels more than one target builds on |

A file sits in the lowest tier that satisfies it. Several console tests link
`pins.cpp` and never link `theme.cpp`, which is the property that keeps them
headless and fast, and it is why `branding.h` is a header-only palette. The
tiers do not link each other: a consumer names every tier it needs.

Qt is dynamically linked and must stay that way.
[THIRD-PARTY-NOTICES.md](../THIRD-PARTY-NOTICES.md) explains what that
obligation is. Adding a fifth Qt module is a licensing decision as well as a
build one.

## The catalogue

`resources/catalog.json` is 2.9 MB of packed vanilla API: 6,108 classes, 29,024
methods, 403 enums, 362 global functions, 578 constants, generated from an index
of the DayZ script tree by `tools/build-catalog.mjs`, which reproduces the
shipped file byte for byte.

Packed means interned. Every string in the file appears once in a `strings`
array and everything else is an integer index into it. `Catalog::load` decodes
that into search rows at startup and builds `NodeDef`s lazily, memoised.
Materialising 29,000 node definitions up front costs more than it saves, and the
lazy build is what keeps startup at 1.3 seconds with the splash on screen at 300
ms.

Three things the catalogue carries that are easy to overlook:

- **Access.** 2,204 methods are protected. `Catalog::accessAllowed` answers
  whether code inside one class may call a member declared on another, and the
  palette applies it, so a node it never offered is a compile error that never
  happened. Private methods are not in the catalogue at all: a `modded class`
  inherits rather than reopens and cannot see them either.
- **Event-ness per class.** Whether a method is an event is decided by walking
  the real inheritance chain through typedefs, not by matching a name across the
  whole tree.
- **Inheritance.** `isA` and `ancestors` are what make a wire between two object
  pins legal or not, and what stops the Events list offering an override the
  class does not inherit.

`tools/catalog_access.mjs` re-proves the access flags against the source index
after a rebuild, and carries a written account of how the catalogue is derived,
because no generator was committed alongside the first one.

## The graph model

`src/graph.h` is the whole model and it is small on purpose. A node stores a
catalogue key or a builtin id, a position, its literal inputs and its options.
Pin shapes are resolved from the catalogue at render time rather than stored, so
a project file stays small and survives a catalogue rebuild after a DayZ update.

Two pin families, with Blueprint semantics: exec pins take one outgoing
connection each and define statement order; data pins are typed, an input takes
one edge and an output may fan out.

`src/pins.h` maps Enforce type names onto pin kinds, colours, default literals
and which inline editor an unconnected input draws. That last one is not
cosmetic: a pin's default is emitted code, so the rule that decides whether a
pin gets an inline field changes what the generator writes, and the painter and
the click handler have to read the same rule.

`src/builtins.cpp` holds the nodes that are not API calls: flow control,
operators, literals, casting, variables, arrays, timing, and the raw Enforce
escape hatch. Four of them write more than statements. Set Timer declares a
`ref` member, constructs it, and writes the callback method, all derived from one
name so the string the engine dispatches on and the method it dispatches to
cannot drift apart.

`src/nodeindex.cpp` is the palette's table of contents: 13 hand-written groups
plus a generated tail, ordered by measured usage rather than taste. It resolves
a curated row by name and owner, not by catalogue key, because keys are indices
into a file that is regenerated whenever the DayZ scripts move. A name either
resolves or does not, and a test says which.

`src/document.cpp` owns session state, selection and undo. Every graph mutation
goes through `beginEdit` and `commitEdit`, so undo is uniform and every panel
refreshes from one signal.

## Code generation

`src/codegen.cpp` walks each event node's exec chain into statements. Data pins
resolve to expressions, pure nodes inline where they are used, and an impure
call that produces a value gets a named temporary.

Three properties worth knowing before changing it:

- **Every intermediate is spilled into its own local.** Not for readability. The
  Enforce compiler mishandles `bool x = <compound expression>;`, which DayZ
  Expansion cites 32 times across 24 files, and the avoidance idiom appears in
  that one corpus at least 101 times. A wire already implies a named value, so
  the generator writes one.
- **Quoting belongs where the user types, not where code is emitted.** The
  lowering legitimately puts real identifiers into pins, so quoting in the
  generator turns working code into `Print("someVar")`.
- **User regions survive.** Text between the `USER_BEGIN` and `USER_END` markers
  is read out of the previous file and written back. The legacy spelling the
  earlier Electron build wrote is still accepted on read, because files it
  generated are still out there.

`GenResult::lineOwners` records which node produced each line, which is what
makes the generated code dock navigable in both directions.

## Reading Enforce Script

Four stages, each one able to fall back without taking the rest down with it.

**`src/enforce/lexer.cpp`** tokenises: keywords and types checked against the
real script tree, block comments across lines, preprocessor, strings with
escapes, hex and exponent numbers. `scanEnforce` reports the identifiers, calls,
members and assignment targets in a block plus its brace and paren balance,
which is how the analyser checks a raw node instead of trusting it.

**`src/enforce/parser.cpp`** builds the syntax tree.

**`src/enforce/lower.cpp`** turns statements into nodes: `if (!m_RestApi)`
becomes a Branch fed by a Not fed by a Get. A statement it cannot lower falls
back to a raw node on its own. A local assigned once is wired directly; a local
assigned more than once becomes a real graph variable, which is what keeps the
result readable.

**`src/enforce/import.cpp`** is the declaration layer above that: the class
header, its members, each method signature. A method body it cannot lower keeps
its text on the function as `rawBody`, so the file regenerates as it was even
where the graph could not model it.

Formatting is part of the model, not an afterthought. A graph carries the
indentation base and unit read off the body, blank lines and comments as trivia
on the node that owns the statement, and the line ending the file was written
with. That last one belongs to the file rather than to a method: 3,776 of the
4,765 files in the corpus are CRLF, and 12 mix both, which is written back as
bare newlines with the import saying so.

## The round trip bar

This is the correctness story and it is the one thing a change has to meet on
purpose.

`importtest` reads every script it can reach, turns each into a graph,
regenerates, and compares. It reports three levels: byte for byte; the same once
the generator's own furniture is set aside; and the same again once spacing
inside a line stops counting. A file this application generated itself is held
to the first.

**A change that moves the byte-for-byte count off zero is refused.** The
arithmetic is why: a file whose line endings changed is a file where every line
changed, so one such file is a diff over a user's whole mod. Somebody opens a
script to look at it, 900 lines are silently rewritten, and their next commit is
unreviewable.

The consequence runs backwards into the lowering. A method becomes nodes only
when regenerating it reproduces it exactly, so a wrong guess is impossible
rather than unlikely. Anything else keeps its text, visible and labelled. That
is also why composite nodes are deliberately not rebuilt by the importer: a
shape spanning a field, two statements and a second method cannot be verified by
regenerating one method, so a near miss would swallow a hand-written callback.
`lowertest` asserts that no composite is produced, so it cannot start half
working by accident.

The corpus floors under that bar (at least half of files coming back line for
line, at least a quarter of methods becoming nodes) have room under them and are
meant to move up. Third party mod code sits at 23% of methods becoming graphs;
the author's own code sits at 64%.

## Analysis

`src/analysis.cpp` runs four rule families over one graph: correctness (dangling
required inputs, type mismatches, cycles, and the balance and name checks inside
raw nodes), DayZ traps (modding a proto native, client and server misuse, sync
variables never marked dirty, `super` skipped, module load order, mutating a
container mid-iteration), dead code, and dependency rules.

It is handed one graph, never the project, and stays quiet about what it cannot
see: a name in a type or static-class position may belong to a sibling script,
so it is never called wrong.

## Reading other people's mods

**`src/pbo/pboreader.cpp`** reads PBO archives in process. No DayZ Tools
installation, which also means it works in a headless test, and the mods already
installed on the machine are the corpus to prove it against.

The format is a header of entries, a zero-length terminator, the data blobs back
to back in the same order, and a 20 byte SHA1 trailer. The compression is LZSS,
but the two-byte token is a distance back into the already-produced output
rather than an index into a ring buffer. Textbook Okumura LZSS decodes the first
20 bytes of a DayZ script correctly and then turns to noise, which is the
failure mode that reads as success. The 4 byte trailer after each stream is the
sum of the decoded bytes and is checked on every read, so a wrong decode is
refused rather than returned.

A mod is a file somebody uploaded, so the reader treats the corpus as hostile
and the corpus earns it: 22,956 entry names containing `..`, 578,059 that look
absolute, 1,034,830 carrying control or reserved characters, one archive with a
150 MB header claiming 1,298,973 entries, and a 2.1 GB archive. Bound the entry
count by `fileSize / 21` rather than a constant or the three genuine
million-entry mods get refused; bound decompression by `dataSize * 9 + 64`, the
ceiling of what LZSS can expand, so a hostile size field cannot force an
allocation.

**`src/modlibrary.cpp`** is the library above it, in two deliberately different
shapes. Scanning is cheap and wide: walk the mod roots, read `mod.cpp` and
`meta.cpp` for a name and an author, read each archive's header for its prefix
and script count, cache the result by folder and modified time, refresh on a
worker thread. Opening is narrow and expensive: extract one mod's `.c` files and
run the importer over each, on the calling thread, because the importer resolves
pins through the catalogue's mutable def cache and racing that against a
repainting UI would be a bug you could not reproduce.

Read only without exception. Nothing writes inside a mod folder, extracted files
land under the application's own cache, and every graph that comes out carries a
flag saying it may not be saved back. One predicate on the graph's own mark
answers for that at every gate, including inside the only function that turns a
graph into a `.c` file.

**`src/config/configtree.cpp`** opens `config.cpp` as a class tree with a
property panel rather than as a graph, because it is declarative. It round trips
by reusing source text whose meaning has not changed, so an edit touches one
line rather than reformatting the file.

## Running the mod

`src/workdrive.cpp` and `src/testrun.cpp` cover the four things between an
exported script and a character standing in a test server: the `P:` junctions,
the mod chain in the Workbench project, the PBO, and the session.

`workdrive.h` splits into a half that reads and a half that acts, and the split
is the safety property. `inspect` reads the disk and decides, writes nothing and
can be called as often as anybody likes. Nothing deletes: a junction is a real
change to somebody's disk and the folder in the way may be the only copy of a
day's work, so the one function that moves a folder refuses unless it has
compared that folder against the mod folder and found nothing in it the mod
folder does not already have.

`testrun.h` is plain logic with no widgets, so the panel above it stays a view.
Every step hands back the command it assembled and everything that came back,
whether or not it worked, because a build that failed with nothing on screen is
the thing that costs an evening. Assembling a command line is also the part that
can be checked without a game installed, which is what `testruntest` does.

## The interface

`src/canvas/` is the scene, node, wire and note items, the view and the minimap.
`src/panels/` is the docks. `src/widgets/` is the dialogs and the reusable
editors. `src/mainwindow.cpp` wires menus, toolbar, tabs, docks and the status
bar together.

Node geometry, colours, wire tangents and the interaction table are in
[DESIGN.md](../DESIGN.md) and that file is the contract. The one thing worth
repeating here is the verification path: `--screenshot out.png` renders the
window offscreen and exits, and several test targets take a `--shot` option, so
the interface can be checked without a person at the keyboard.

## The documentation generator

`tools/nodedoc/` writes [node-reference.md](node-reference.md) out of the same
tables the application reads, and prints the Enforce that a `.sdzn` generates.
The reference is generated rather than written because it would otherwise be a
fourth copy of the node tables and would be wrong the first time somebody added
a node.

It is configured on its own rather than as a target of the root project. It
wants Qt Core and Qt Gui only, it is run by whoever is editing the docs rather
than by every build, and keeping it out of the root `CMakeLists.txt` means a
documentation change cannot break the application's build.

```
cmake -S tools/nodedoc -B build/nodedoc -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64 -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
cmake --build build/nodedoc
build/nodedoc/nodedoc --reference docs/node-reference.md
build/nodedoc/nodedoc --generate docs/examples/examples.sdzn
```

Run the first of those and commit the result after changing `src/nodeindex.cpp`
or `src/builtins.cpp`. It refuses to write a file containing anything outside
printable ASCII and names the character and the line, because the output is
committed documentation and the house rule is plain ASCII.

Two other tools sit beside it. `tools/ascii_sweep.py` reports and fixes
typographic characters across the tree, leaving em-dashes for a person to
decide. `tools/build-catalog.mjs` regenerates the catalogue.

## The working notes

[notes/](notes/) holds the research and engineering notes behind several of the
decisions above, kept because the measurements in them are the reason things are
the way they are.

- [notes/expansion.md](notes/expansion.md) is DayZ Expansion read against the
  files: how the largest DayZ mod there is actually organised, and the compiler
  hazards it writes around. The palette's group ordering comes from it.
- [notes/engineering-notes.md](notes/engineering-notes.md) is the environment,
  the traps in this codebase that keep biting, the two ways to test a mod, and
  the PBO corpus measurements.
- [notes/custom-nodes.md](notes/custom-nodes.md) is a design note for work that
  is not built yet.
