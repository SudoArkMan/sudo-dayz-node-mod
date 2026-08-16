# Contributing

Thanks for looking. This is a tool for people who write DayZ mods, and the most
useful contributions come from people who have hit the thing they are fixing.

Before writing code, read `DESIGN.md` for the interface contract and
`docs/architecture.md` for how the parts fit together.
`docs/notes/engineering-notes.md` has the traps in this codebase that keep
biting. All three are short and all three will save you a wasted afternoon.

## What you need

| | |
| --- | --- |
| Qt | 6.11.0, MinGW 13.1 build. `C:/Qt/6.11.0/mingw_64` is the reference install. |
| Compiler | The MinGW GCC that ships with Qt, `C:/Qt/Tools/mingw1310_64/bin/g++.exe` |
| CMake | 3.16 or newer |
| Generator | Ninja |
| Standard | C++17 |

Qt modules are Core, Gui, Widgets and Network. Nothing else. Network is there
for one thing, the opt-in update check in `src/update.cpp`. If a change needs a
fifth Qt module, say why in the pull request: the module list is part of the
licensing position in `THIRD-PARTY-NOTICES.md` and of what a release has to
deploy, not just a build detail.

Qt must stay dynamically linked. That is a licence requirement, not a
preference, and `THIRD-PARTY-NOTICES.md` explains what breaks if it changes.

Some test suites read a real DayZ installation and a mounted `P:` work drive.
They skip themselves when those are absent, so a machine without DayZ Tools can
still build and run everything. What you cannot do on such a machine is move
the corpus numbers, because there is no corpus.

## Build

Configure once:

```
cmake -S . -B build/cli -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64 -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
```

Build:

```
cmake --build build/cli
```

Run it on the sample project:

```
build/cli/DAYZSUDONodeMod.exe resources/SUDO_Link.sdzn
```

`--screenshot out.png` renders the window and exits, which is how the interface
gets checked without a person at the keyboard.

**Close the application before you build.** A running `DAYZSUDONodeMod.exe`
holds its own binary open. The link fails, Ninja stops before it relinks the
test targets, and every measurement after that is read off a stale binary that
still passes. This has cost real time. If a number looks impossible, check
whether the build actually succeeded.

Sources are compiled once each through four object libraries, not once per
target. The split is load bearing and the comments in `CMakeLists.txt` explain
which tier a new file belongs in. The short version: a file goes in the lowest
tier that satisfies it, and several console tests link `pins.cpp` without ever
linking `theme.cpp`. Do not collapse the tiers.

## Tests

There are 19 suites. Build them all with the ordinary build, then run each with
the resources directory as its argument:

```
cmake --build build/cli
for f in build/cli/tests/*.exe; do "$f" resources || echo "FAILED: $f"; done
```

One at a time:

```
cmake --build build/cli --target coretest
build/cli/tests/coretest.exe resources
```

| Suite | What it holds down |
| --- | --- |
| `coretest` | catalogue decode, inheritance, search, pin types, project round trip |
| `crashtest` | the four crash repros: cycles, nesting depth, Sequence fan-out |
| `eventstest` | event ranking and that no event disappears in the reorder |
| `templatetest` | the bundled mod template copies byte for byte with the name substituted |
| `lexertest` | the Enforce tokeniser, against real vanilla source when `P:` is mounted |
| `parsertest` | the Enforce parser, and the share of statements it has to keep as raw |
| `importtest` | file to graph and back. See "the round trip bar" below. |
| `lowertest` | code to nodes and back, and how much of a project stops being a text box |
| `mathtest` | arithmetic reachable from the palette and surviving into the file |
| `depstest` | mod dependencies, the CF and COT presets, addon names out of a `config.cpp` |
| `deprulestest` | the DZ314, DZ315 and DZ316 dependency rules |
| `configtest` | `config.cpp` parsed and written back byte for byte |
| `configeditortest` | the config editor's rules and what a save writes |
| `valueeditortest` | one widget per Enforce kind and the literal it hands back |
| `modlibrarytest` | installed mods as a library, and the read only promise |
| `pbotest` | PBO archives read without DayZ Tools, synthetic and against the corpus |
| `recenttest` | the recent projects list and the start page built on it |
| `testruntest` | prerequisite detection, junctions, and the command lines, without launching a game |

The suites that build a window force the offscreen platform themselves, so the
whole set runs with no display.

A few take arguments worth knowing:

```
build/cli/tests/pbotest.exe resources --all
build/cli/tests/importtest.exe resources -v
build/cli/tests/modlibrarytest.exe resources 6
```

## The round trip bar

This is the project's correctness story, and it is the one thing a pull request
has to meet on purpose rather than by luck.

The application opens somebody else's `.c` file, turns it into a graph, and
writes a `.c` file back. Those two files have to say the same thing. The
generator is the inverse of the importer, so the claim is measurable rather
than argued: read a file in, generate a file out, compare.

`importtest` prints three numbers for the vanilla and installed-mod corpus:

| | |
| --- | --- |
| exact | byte for byte, original against generated |
| restored | the same, once the generator's own furniture is set aside: the preserved region it writes into every file, its brace placement, blank lines |
| same code | the same again, once spacing inside a line stops counting |

Anything else that changed is a loss and the run names it.

A file this application generated itself is held to the first of the three. It
has to come back **byte for byte**, because clicking a script must not rewrite
it. `importtest` reports that as scripts that "come back byte for byte", and
the check is `moved == 0`. It runs twice: once with bare newlines, and once
with the whole file rewritten to CRLF, because three quarters of the installed
mods are CRLF files.

**A pull request that moves `importtest` off zero changed files will be
refused.** Not reduced, not explained. Zero. The reason is arithmetic rather
than fussiness: a file whose line endings changed is a file where every line
changed, so one such file is a diff over a user's whole mod. Somebody opens a
script to look at it, the tool silently rewrites 900 lines, and their next
commit is unreviewable. That is worse than the feature being missing.

The same rule covers the line-ending checks in the same suite:
`endingLost == 0`, every class whose file ends its lines one way comes back
ending them the same way.

The corpus floors below that (at least half coming back line for line, at least
a quarter of methods becoming nodes) are floors with room under them. They are
allowed to move up. Moving them up is most of what there is to do.

If your change lowers more code into nodes, say the before and after numbers in
the pull request. `lowertest` and `importtest` both print them.

## House style

The code and the interface are written in one voice, and it is worth matching.

- Plain ASCII. No em-dashes, no curly quotes, no ellipsis characters, no
  arrows, no middots. There is a sweep for this in `tools/ascii_sweep.py`, and
  the catalogue generator was fixed once for emitting 2,867 middots into
  strings the inspector shows.
- Sentence case in the interface. No all-caps, no wide letter spacing.
- Comments explain why a thing is the way it is, especially where the obvious
  implementation is wrong. Several files carry a paragraph at the top saying
  what would break if somebody tidied them; those paragraphs are the point.
- Claims in comments and documentation are checkable. If you write a number,
  say what produced it.

## Pull requests

- One change per pull request. The suites are fast; small changes are easy to
  bisect and easy to refuse without refusing everything else.
- Run all 19 suites and paste the result. The pull request template asks for it
  because a green suite is the only claim anybody can check quickly.
- If you touched anything the interface draws, attach a screenshot. The
  `--screenshot` flag and several suites' `--shot` options exist for this.
- If you changed `src/nodeindex.cpp` or `src/builtins.cpp`, regenerate
  `docs/node-reference.md` and commit it. That file is written out of those
  two tables rather than by hand, so it goes stale the moment a node is
  added. The generator and its build line are in `docs/architecture.md`.
- Do not commit into `build/`, and do not commit deployed Qt DLLs. `.gitignore`
  covers both.
- Do not change line endings. `.gitattributes` freezes the fixtures under
  `resources/` for a reason: they are the input to tests that compare bytes.

## Reporting a bug instead

If you are not writing the fix, that is fine and still useful. Use the bug
report template, and include the version, what you had open, and the mod or
script that provoked it if you can share it. Security issues go through
`SECURITY.md` rather than the issue tracker.
