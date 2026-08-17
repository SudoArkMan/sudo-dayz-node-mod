# How these screenshots were taken

Every picture in `docs/` is produced by the application rendering itself and
exiting. Nothing here was captured with a screenshot key, so a picture can be
redone exactly when the interface changes, and a picture that has rotted is a
picture whose command no longer produces it.

Redo the whole set after any change to the docks, the theme, the palette groups
or the start page.

## Before you start

```
export PATH="C:/Qt/Tools/mingw1310_64/bin:/c/Qt/6.11.0/mingw_64/bin:$PATH"
cd <repository root>
cmake --build build/cli
```

Close the application first. A running `DAYZSUDONodeMod.exe` holds its own binary
open, the link fails, and the shot is then taken from a stale build.

The commands below assume a shell variable for the output folder:

```
S=docs/images
E=./build/cli/DAYZSUDONodeMod.exe
P=docs/examples/first-mod.sdzn
```

## The hooks

`--screenshot <png>` renders the window offscreen, paints any open menu into the
grab, saves and exits. Six environment variables drive the window into a state
first, and all six are unset in a normal run.

| | What it does |
| --- | --- |
| `SUDO_UI_SIZE=1280x800` | Resizes the window before the docks divide the height. The default is 1600x950, which is too wide to read in a browser |
| `SUDO_UI_SEARCH=<query>` | Types a query into the Node Palette search box |
| `SUDO_UI_SELECT=<node id>` | Selects a node by its id in the `.sdzn` |
| `SUDO_UI_BROWSE=<mod>[#row]` | Opens the Mod Browser on an installed mod and opens the class at that row. Also takes a path on disk |
| `SUDO_UI_MENU=<title>` | Drops a menu bar menu open, matched on its title |
| `SUDO_UI_POPUP=<kind>` | Opens a canvas popup: `add`, `event`, `connect`, `connect-exec`, `scripts`, `tabmenu` |

Read `main.cpp` and `MainWindow::showEvent` for what each one accepts. They are
the source of truth, not this table.

## Before the shot: the recent projects list

The start page shows the recent projects of whoever took the picture, paths and
all. Reduce it to the projects that ship with this repository before taking
`start-page.png`, and put the real file back afterwards:

```
%APPDATA%\SUDO\SUDO DayZ Node Mod\recent-projects.json
```

The published picture lists `MyFirstMod` (`docs/examples/first-mod.sdzn`),
`Node Showcase` (`resources/Showcase.sdzn`) and `SUDO Link`
(`resources/SUDO_Link.sdzn`). All three exist in a clone, so none of them draws
the orange "not at this path" line.

Two more things about that list. The relative times in it are worked out from
`lastOpened`, which is UTC: a timestamp in the future prints as an absolute date
instead of "6 minutes ago", so keep them behind the clock. And a mod folder
opened through `SUDO_UI_BROWSE` on a path is added to
`modlibrary.json` beside it, which puts the machine's mod count up by one in the
status bar. Take it back out before a shot that shows that line, or the number
stops matching the rest of the set.

`start-page.png` also needs the update question in its unanswered state, which is
the state a fresh install is in. The answer lives in the application's own
settings, in the registry under
`HKCU\Software\SUDO\SUDO DayZ Node Mod\updates`. No value there means nobody has
answered, which is what the picture shows. On a machine that has already
answered, the picture has to be retaken elsewhere or that value cleared.

## The two that go stale first

`start-page.png` carries the template gallery, which is copy and layout rather
than structure and moves whenever the templates are reworded. `browsing-a-mod.png`
carries the status bar's warning count, which moves whenever an analysis rule is
added. Both were retaken against the build of 17 Aug 2026 after exactly those two
things drifted. Check them first when the set is being redone.

## The raw captures

Each line writes one 1280x800 PNG into a scratch folder. The crops below come out
of these.

| Capture | Command |
| --- | --- |
| `start-page.png` | `SUDO_UI_SIZE=1280x800 $E --screenshot start-page.png` |
| `editor-window.png` | `SUDO_UI_SIZE=1280x800 $E --screenshot editor-window.png $P` |
| `inspector-print.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_SELECT=n2 $E --screenshot inspector-print.png $P` |
| `palette-search.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_SEARCH=print $E --screenshot palette-search.png $P` |
| `menu-file.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_MENU=File $E --screenshot menu-file.png $P` |
| `menu-test.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_MENU=Test $E --screenshot menu-test.png $P` |
| `popup-add.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_POPUP=add $E --screenshot popup-add.png $P` |
| `popup-connect.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_POPUP=connect $E --screenshot popup-connect.png $P` |
| `showcase.png` | `SUDO_UI_SIZE=1280x800 $E --screenshot showcase.png resources/Showcase.sdzn` |
| `mod-browser.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_BROWSE="3D Printer#2" $E --screenshot mod-browser.png` |
| `mod-browser-raw.png` | `SUDO_UI_SIZE=1280x800 SUDO_UI_BROWSE="3D Printer#1" $E --screenshot mod-browser-raw.png` |
| `wrong-event.png` | `SUDO_UI_SIZE=1280x800 $E --screenshot wrong-event.png <begin-probe.sdzn>` |

`n2` is the Print node in `docs/examples/first-mod.sdzn`. If that file is
renumbered, the id in the command changes with it.

That project's event node names a catalogue key, `m3783`, which is
`Mission::OnInit` in the catalogue this build ships. Catalogue keys are indices
into `resources/catalog.json`, so a rebuild after a DayZ update can move them.
If the node opens as something other than Event OnInit, find the key again and
put it back:

```
node -e "const d=require('./resources/catalog.json');const S=d.strings;
  const c=d.classes.findIndex(x=>S[x[0]]=='Mission');
  d.methods.forEach((m,i)=>{if(m[0]==c&&S[m[1]]=='OnInit')console.log('m'+i)})"
```

It has to be the declaration carrying the event flag, which is the one on
`Mission`. `MissionServer` has its own `OnInit` entry, and a node pointing at
that one draws as a call rather than as an entry point.

The two `SUDO_UI_BROWSE` lines name a mod installed on the machine the pictures
were taken on. Any mod with script does, and the `#N` suffix picks the row in the
class list: `#2` is a class with a mix of nodes and raw text, `#1` is one that is
mostly raw. Pick rows that show that difference rather than a specific mod.

`begin-probe.sdzn` is `docs/examples/first-mod.sdzn` with the event node replaced
by a Begin builtin, which is the mistake the picture documents:

```json
{ "id": "n1", "kind": "builtin", "ref": "bi.begin", "x": 0, "y": 0, "inputs": {} }
```

## The published files

Crops and one annotation, all from the captures above. Coordinates are in the
1280x800 frame. Upscaled crops use Lanczos at 2x; everything is written through a
256 colour palette, which is lossless enough for a flat dark interface and roughly
halves the file. The whole set is under 300 KB, which matters because this is a
public repository and a history full of megabyte PNGs is somebody's slow clone.

| Published | From | How |
| --- | --- | --- |
| `start-page.png` | `start-page.png` | whole window |
| `update-consent.png` | `start-page.png` | crop 24,392 to 700,630 |
| `editor-tour.png` | `editor-window.png` | whole window, plus ten numbered badges |
| `first-graph.png` | `editor-window.png` | crop 372,218 to 890,428, 2x |
| `generated-code.png` | `editor-window.png` | crop 0,565 to 1280,782 |
| `node-and-line.png` | `inspector-print.png` | whole window |
| `palette-search.png` | `palette-search.png` | crop 0,205 to 320,375, 2x |
| `events-list.png` | `showcase.png` | crop 0,380 to 320,562, 2x |
| `variable-manager.png` | `showcase.png` | crop 900,60 to 1280,285, 2x |
| `add-node-search.png` | `popup-add.png` | crop 516,168 to 958,574 |
| `wire-search.png` | `popup-connect.png` | crop 516,168 to 958,574 |
| `file-menu.png` | `menu-file.png` | crop 0,0 to 470,462 |
| `test-menu.png` | `menu-test.png` | crop 0,0 to 470,240 |
| `browsing-a-mod.png` | `mod-browser.png` | whole window |
| `raw-nodes.png` | `mod-browser-raw.png` | crop 0,88 to 900,230 |
| `wrong-event.png` | `wrong-event.png` | crop 0,565 to 1280,782 |

The badges on `editor-tour.png` are filled circles, radius 13, in `#f2b134` with a
2px `#14181e` outline and the number in bold Segoe UI 17, centred at:

```
1 (250,72)   2 (250,215)  3 (250,391)  4 (610,105)  5 (1120,72)
6 (1120,298) 7 (1120,460) 8 (610,577)  9 (700,44)   10 (760,788) radius 11, 14pt
```

Their positions are the panel title bars in that frame. Change the dock layout and
they move.

`editor.png` and `mod-browser.png` are older 1600x950 captures used by the top
level `README.md`. They are not part of the set above and are left alone.

## What cannot be captured today

Two things the guide needs and the hooks do not reach. Both are noted here rather
than worked around, so nobody spends an afternoon rediscovering them.

- **The Test dock.** It is tabbed behind Generated Code at the bottom, no hook
  raises it, and the layout is not persisted between runs, so a fresh window
  always opens with Generated Code in front. `tests/testruntest.cpp --shot
  <png>` grabs the real panel on a real scaffolded mod, but the text in that grab
  renders as empty boxes: the test binary paints without a usable font. The guide
  uses `test-menu.png`, which carries the same actions and their shortcuts, and
  describes the dock in prose.
- **Modal dialogs.** New mod, New script, Export scripts and the work drive
  report are modal, and no hook opens one. The guide gives their fields as tables
  instead. The mods picker is the one exception, through
  `testruntest --shot-mods <png>`, and it comes back with the same missing font
  as the dock above.
