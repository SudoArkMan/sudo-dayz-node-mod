# Changelog

All notable changes to this project are recorded here.

The format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and the project follows [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Numbers quoted below were measured by the test suites named beside them, not
estimated.

## [Unreleased]

### Added

- Update check. The application can ask the GitHub releases API whether a newer
  version has been published, compare it by semantic version order rather than
  string order, and show what changed. It contacts nothing until the user has
  agreed to it, and it does not download or install anything.

## [0.1.0] - 2026-08-16

First public release.

### Added

**Node editor**

- Node canvas with a graph model, connection rules and undo. Nodes are sized to
  their content, so a catalogue call carrying `plugin = LOG_DEBUG` shows the
  parameter and its default instead of truncating both.
- Live generated code view beside the graph.
- Variables, outliner, inspector, events and problems panels.
- Notes on the canvas, minimap, and a palette in 13 groups written as jobs
  rather than engine subsystems, ordered by how often real mods use each one.
  The "Everything else" group is generated from the builtins, so a new node
  cannot go missing from the palette.

**Enforce Script**

- Lexer and parser for Enforce Script, checked against the vanilla script tree.
- Code to nodes: a method becomes a graph only when the graph regenerates it
  byte for byte, so a wrong guess is impossible rather than unlikely. On third
  party mods 23% of methods become node graphs and the rest keep their text
  (`modlibrarytest`); on the author's own code the figure is 64%.
- Graphs carry the source's own formatting: indentation as a base and unit pair
  read off the body, and blank lines and comments as trivia on the node that
  owns the statement.
- Line endings belong to the file rather than to a method, so a CRLF script
  comes back CRLF. 3,776 of the 4,765 files in the corpus are CRLF. A file that
  mixes both is written with bare newlines and the import says so; 12 of 4,765
  mix.
- A carriage return that ends no line is left where it was put, so
  `Print("a\rb")` survives a round trip.
- Code generation with user regions preserved, and file assembly (preamble,
  the join between two classes, the trailing ending) in one place a test can
  reach.

**Node catalogue**

- Every vanilla class, method, enum, global function and constant is a node:
  6,108 classes, 29,024 methods of which 2,193 are events and 8,392 pure, 403
  enums, 362 globals, 578 constants. Strings are interned in the packed file and
  node definitions are built lazily, so startup does not materialise 29,000 of
  them.
- Method access is recorded, and the palette will not offer a call the generated
  script would not be allowed to make. 2,204 catalogued methods are protected;
  a protected method stays available where the graph inherits it and disappears
  only as a call on an unrelated object. Private methods are not catalogued.
- Event-ness is decided per class by walking the real inheritance chain through
  typedefs, rather than by method name across the whole tree. 1,410 methods were
  reclassified by that change and `ItemBase` went from 227 events to 263.
- `tools/build-catalog.mjs` generates the catalogue and reproduces the shipped
  file byte for byte.

**Analysis**

- Correctness, DayZ trap and dead code diagnostics, each with a DZ code, shown
  against the node that caused them.
- Dependency rules covering Community Framework, Community Online Tools and
  Dabs Framework, with addon names read out of each framework's own
  `config.cpp` rather than recalled. COT requires CF, so a mod depending on COT
  pulls both.

**Reading other people's mods**

- Native PBO reader, no DayZ Tools installation needed. Measured over the
  installed mods: 1,874 archives across 253 mods, 9,343,981 packed entries
  decoded and checksum matched, 0 refused, 575.7 MB in 33.8 s (`pbotest --all`).
  Checked against `BankRev.exe` as an outside oracle on 648 entries, 0 differed.
- Mod browser over the installed mods, 266 of them, each class opening as a
  graph. A browsed graph is marked read only, and one predicate on the graph's
  own mark answers for that at every gate including inside the only function
  that turns a graph into a `.c` file.
- A loose `.pbo`, a mod folder or an unpacked script tree all open the same way.

**config.cpp**

- `config.cpp` opens as a class tree with a property panel rather than as a node
  graph, because it is declarative. 243 of 243 configs on the work drive round
  trip, by reusing source text whose meaning has not changed, so an edit touches
  one line.
- Validation catches a leftover `CfgPatches` name, a `files[]` path with no
  folder behind it, and `dir` not matching the mod folder.

**Projects and scaffolding**

- `.sdzn` project format, shared with the earlier Electron build so projects
  move between the two. Unknown fields are preserved on load and save.
- Start page with recent projects, new, open, and a templates gallery built
  from the same script skeletons the New script dialog uses.
- Autosave writes a sidecar beside the project every two minutes, so its
  relative paths still resolve, and recovery never writes the project itself.
- New mod scaffolding from the bundled DayZ mod template, with the `CfgPatches`
  class renamed so two mods built from the template do not collide at load.

**Running the mod**

- Test dock that builds the PBO and launches a DayZ diag server and client,
  showing the command and its output at every step.
- Offline launching sits beside the dev server pair as a first class choice,
  with a list of what offline cannot show. That list is checked against vanilla
  and against COT's own `scripts.pbo`: COT permissions fail open offline, not
  closed, because `HasPermission` returns true under `IsMissionOffline()`, so a
  permission gated feature always opens on a single player run and can still
  lock players out on a server.

**Interface**

- Splash over the real startup stages, multi-size executable icon, toolbar
  corner mark, and a theme on the brand palette with pin colours that follow it.
  Startup is 1.3 s, with the splash on screen at 300 ms.
- `--screenshot out.png` renders the window and exits, which is how the
  interface is checked without a person at the keyboard.

**Build and tests**

- CMake and Ninja build against Qt 6.11 with MinGW 13.1, linking Qt Core, Gui
  and Widgets only.
- Shared sources compiled once each through four object libraries, split so the
  console test targets link `pins.cpp` without pulling in Qt Widgets.
- 19 test suites, all headless. The suites that need a window force the
  offscreen platform themselves.

### Known limitations

- Windows only. The application reads the Windows registry and Steam library
  folders to find DayZ and DayZ Tools, and creates junctions on the `P:` work
  drive.
- Some suites need a DayZ installation and a mounted `P:` drive. They skip
  themselves when those are absent, which also means the corpus numbers above
  cannot be reproduced without them.
- Composite nodes are deliberately not rebuilt by the importer. A shape spanning
  a field, two statements and a second method cannot be verified by regenerating
  one method and comparing, so a near miss would swallow a hand-written callback
  and rewrite it on the next generate. `lowertest` asserts that no composite is
  produced, so it cannot start half working by accident.
