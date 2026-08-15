# SUDO DayZ Node Mod

Visual scripting for DayZ Enforce Script, in native Qt. Every vanilla class,
method, enum, global function and constant is a node; the graph generates
compilable Enforce Script.

A Qt Widgets rebuild of the Electron prototype at
`C:\Users\dilla\SUDO DayZ Script Node`. Both share the `.sdzn` project format,
so projects move between the two in either direction.

## Build

The Qt Creator kit (Qt 6.11, MinGW 13.1) is already configured. From the
command line:

```bash
cmake -S . -B build/cli -G Ninja -DCMAKE_PREFIX_PATH=C:/Qt/6.11.0/mingw_64 -DCMAKE_CXX_COMPILER=C:/Qt/Tools/mingw1310_64/bin/g++.exe
```

```bash
cmake --build build/cli
```

Run it with a project:

```bash
build/cli/DAYZSUDONodeMod.exe resources/SUDO_Link.sdzn
```

`--screenshot out.png` renders the window and exits, which is how the UI gets
checked without a human at the keyboard.

## Layout

```
main.cpp              startup, catalogue load, --screenshot
DESIGN.md             UI contract: dock layout, theme, node spec, interaction
src/
  pins.*              Enforce type -> pin kind, colours, inline editors
  graph.*             node/edge model, connection rules, .sdzn JSON
  catalog.*           packed catalogue: lazy node defs, search, explain()
  project.*           .sdzn load/save, unknown fields preserved
  builtins.*          flow, operators, literals, cast, variables, raw
  codegen.*           graph -> Enforce Script, user regions preserved
  analysis.*          correctness / DayZ traps / dead code diagnostics
  document.*          session state, selection, undo, change signals
  theme.*             palette + stylesheet
  canvas/             scene, node/wire/note items, view, minimap
  panels/             palette, outliner, variables, inspector
tests/coretest.cpp    headless model-layer test
resources/
  catalog.json        generated node catalogue (2.9 MB)
  SUDO_Link.sdzn      sample project, 25 scripts
```

## Where the nodes come from

The palette is generated, not hand-written. `resources/catalog.json` is packed
from the index produced by the **dayz-script-api** skill:

| | |
| --- | --- |
| Classes | 6,108 |
| Methods | 29,024, of which 2,144 events and 8,392 pure |
| Enums 403, Globals 362, Constants 578 | |

Refresh it after a DayZ update by rebuilding the index and re-running the
Electron project's `npm run catalog`, then copying `catalog.json` across.

Strings are interned in the packed file, so `Catalog` decodes it into search
rows once at startup and builds `NodeDef`s lazily. Materialising 29k node
definitions up front would cost more than it saves.

## Tests

```bash
cmake --build build/cli --target coretest
```

```bash
build/cli/tests/coretest.exe resources
```

Checks that the catalogue decodes, inheritance and search behave, pin type
parsing handles the generic forms, and a real 25-script project survives a
save/load round-trip with every node and edge intact.
