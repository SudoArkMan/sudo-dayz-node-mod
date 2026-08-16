# Getting started

From an empty window to a mod loaded in a running test server. Every step here
is one the application actually implements, in the order it implements them, and
the places that bite are called out where you meet them rather than at the end.

## Before you start

| | Needed for |
| --- | --- |
| Windows | Everything. The application reads the registry and Steam library folders, and makes junctions on `P:` |
| The application and its `resources` folder | Everything. It will not start without `resources/catalog.json` |
| DayZ | Launching a test |
| DayZ Tools | Packing a PBO, and unpacking the vanilla scripts |
| A mounted `P:` work drive | Packing a PBO, and launching |

You can wire graphs and generate script with nothing but the application. The
last three are for the half of the loop that puts the mod in the game.

### The work drive

DayZ's tools resolve every path through a drive letter, `P:`. Binarize,
AddonBuilder and Workbench all do it, so a mod folder that is not reachable at
`P:\<YourPrefix>` cannot be packed at all, whatever else is correct.

DayZ Tools mounts `P:` when you open it. If you would rather not open it, the
same thing is `subst P: <your work drive folder>` from a command prompt, and it
does not survive a reboot either way. The application checks and tells you which
of the two problems you have: the drive is not mounted, or the drive is mounted
and your mod is not linked into it.

The link itself is made for you. A mod scaffolded by **File > New mod...** is
junctioned as part of being created, and for a mod folder that already exists
there is **Set up work drive** in the Test dock, shortcut `Ctrl+Shift+P`.

Two things the application will refuse, both on purpose:

- **A prefix the work drive already uses.** `DZ` and its neighbours hold the
  game's own unpacked data. A junction with one of those names shadows vanilla,
  and the failure it produces looks like nothing to do with you.
- **Deleting anything.** If a real folder is already sitting at `P:\<Prefix>`,
  it is left exactly where it is. The application will offer to rename it aside
  only when it has compared it against your mod folder and found nothing in it
  that your mod folder does not already have.

## 1. Start the application

The start page opens on the recent projects, the ways to start something new,
and the templates.

| | |
| --- | --- |
| New project | A bare graph with nothing on disk behind it |
| New mod | The whole mod folder from the bundled template, then a project inside it |
| Open project | Any `.sdzn` on this machine |
| Read a mod | Any of the mods installed on this machine, or a loose `.pbo` |

**New mod** is the one to pick if you are starting from nothing. It is the only
one that leaves you with something the game can load.

If you would rather read than build first, **Read a mod** opens the mod browser
over what is already installed. Everything it opens is marked read only and
cannot be written into your own mod, so it is a safe place to look around.

## 2. Create the mod

**File > New mod...** asks for four things and shows you the folder it is about
to write before it writes anything.

| Field | What it becomes |
| --- | --- |
| Prefix | The folder name, the `CfgPatches` class, the PBO name, and the junction at `P:\<Prefix>` |
| Display name | What the launcher shows |
| Author | Written into `config.cpp` |
| Location | Where the mod folder is created |

The prefix has to start with a letter and hold only letters, digits and
underscores, because it has to survive being a class name, a folder name and a
PBO name at once. `ModTemplate` itself is refused: that is the token the template
replaces, and leaving it would collide with every other mod built the same way.

Missions are optional and off by default. Most mods do not ship one, and the
mission folders are the bulk of the template. Turn them on if you want a mission
of your own to launch against, and pick the maps.

What you get is a complete mod folder: `config.cpp` with the script modules
registered, `$PBOPREFIX$`, a Workbench project, a stringtable, a `Scripts`
folder under the four script modules, and the junction to `P:`.

## 3. Add a script

A script is one class in one file. Use the Mod Explorer's **New file**, or
**File > New project** and then add scripts to it.

The new script dialog asks for a class name and a starting shape, and the shape
it offers depends on which script module the file lands in, because a script in
`4_World` and a script in `5_Mission` reopen completely different things.

| Shape | What it writes |
| --- | --- |
| New class | A class of your own with a constructor and destructor |
| Modded class | `modded class <base>` with one example override in place |
| Empty | Nothing at all |

If you are not sure, `4_World` with a modded `ItemBase` is where most item
behaviour lives, and `5_Mission` with a modded `MissionServer` is where
server-side setup goes.

## 4. Wire something

The canvas is the graph. Nodes come from four places, all of which reach the
same catalogue:

- **The Node Palette**, arranged by what you are trying to do rather than by
  engine subsystem. Start there if you do not know the name of the thing you
  want. [node-reference.md](node-reference.md) is the same list on a page.
- **Search** in the palette. Two words work: `config get` finds
  `ConfigGetString`.
- **Right-click the canvas** for the add-node search at that point.
- **Drag a wire into empty space** for the same search, filtered to what can
  connect to the pin you dragged from.

The Events list is worth knowing about separately. It shows the hooks the class
you are in can override, ranked, with the method each one becomes. An Event node
is an override and nothing else, so the list only offers events your class
actually inherits. Taking one from an unrelated class would generate a method
with the right name on the wrong class, which compiles and is never called.

Two pin families. Exec pins are triangles and decide the order statements run
in. Data pins are circles, coloured by type, and carry values. An unconnected
data input draws a box you can type a value into, so a graph with nothing wired
still generates something that compiles.

The smallest useful graph is three nodes: **Begin**, **Server Only**, and
something that changes the world. Four worked examples with the script each one
produces are in [examples/](examples/).

## 5. Read what it generates

The Generated Code dock under the canvas updates as you edit. It is the actual
file, not a preview of one, and it is navigable in both directions: put the
cursor on a line and the node that produced it is selected, select a node and
its lines scroll into view.

**Validate** on the toolbar, or `F8`, runs the analysis: correctness problems,
DayZ traps and dead code, each with a code and each shown on the node that
caused it. The status bar carries the counts.

## 6. Write the files

**File > Export scripts...** writes the `.c` files. Scripts that were imported
from a file go back to that file; the rest go into the mod's `Scripts` folder
under the right module, and the dialog says which is which before it writes.
Read-only scripts opened out of somebody else's mod are never written anywhere.

**This is the step that is easy to skip.** The generated code dock is live, so
it always looks current, but nothing is on disk until you export. The PBO is
packed from the folder, not from the project, so an export you did not do is a
PBO built from your last export.

Anything you hand-wrote between the user-region markers in a generated file is
read back and carried across, so exporting again does not throw away helpers you
added by hand.

## 7. Build the PBO

The Test dock is behind the Generated Code dock at the bottom, and the same
actions are on the **Test** menu.

**Build PBO**, or `F9`, does two things: it writes the mod chain into the
Workbench project, then runs AddonBuilder over the folder holding `config.cpp`
and puts the result in `P:\Mods\@<Prefix>\Addons`, which is the only place the
engine looks for a mod that did not come from the Workshop.

The dock prints the command it ran and everything that came back, including when
the step worked. A build that failed silently is the thing that costs an
evening, so nothing is swallowed.

If a step is blocked, the checklist above the buttons says which prerequisite is
missing and what to do about it. **Set DayZ Tools folder...** is there for a
machine where the registry key is not set, which is common.

## 8. Launch a test

**Launch test**, or `F5`. The selector beside it chooses between two runs that
are not interchangeable.

**Offline** is one `DayZDiag_x64.exe` on the mod's own mission. It comes up in a
fraction of the time, which is what you want while a script is changing every
minute.

**Dev server** is two processes: a diag server first, then a diag client
connecting to it once the port is open. It is the only one of the two that is
actually a server.

Both run with `-filePatching`, because a script mod is unloadable without it,
and both use `DayZDiag_x64.exe`. The retail `DayZ_x64.exe` and
`DayZServer_x64.exe` both stop at the loading screen once file patching is on.

**Stop**, or `Shift+F5`, kills what the application started, server first.

### What offline cannot tell you

The dock prints this list beside the selector, and every line on it was checked
against the vanilla scripts or against the mod's own source:

- **Another player.** One process, one character.
- **Anything networked.** `IsMultiplayer()` is false offline, and vanilla gates
  467 checks across 197 files on it, RPC delivery included.
- **A variable written on the wrong side.** `EntityAI.IsServerCheck` only raises
  its error for a multiplayer client.
- **Code behind `IsDedicatedServer()` or `#ifdef SERVER`.** Offline is neither.
- **Anything set in `server.cfg`.** Offline passes no `-config`, so
  `ServerConfigGetInt` finds nothing and `cfggameplay.json` is not loaded.

One of these deserves being said twice. If you load Community Online Tools,
permissions offline **fail open, not closed**: `HasPermission` returns true
whenever the mission is offline, and roles load only on a multiplayer server. A
permission-gated feature therefore always works on your machine and can still
lock every player out on the server.

## 9. Depending on another mod

A mod written against Community Framework, Community Online Tools or Dabs
Framework has to declare it, and has to be tested with it loaded. Declaring a
dependency in the project is what writes it into the mod chain the test session
launches with.

The order is the part that bites. The engine loads `-mod=` left to right and a
mod must come after everything it needs, so the chain is sorted from what each
entry declares rather than from the order you added them. COT requires CF, so
depending on COT pulls both.

**Choose mods...** in the Test dock loads other installed mods beside yours for
one run without declaring them, which is what you want for a mod you are testing
against rather than depending on.

## When it goes wrong

| Symptom | Usually |
| --- | --- |
| The application will not start and says so | `resources/catalog.json` is missing or unreadable |
| Build PBO is greyed out | `P:` is not mounted, or the mod is not junctioned into it |
| AddonBuilder cannot find a path | The junction points somewhere else, or the mod folder moved |
| The PBO builds but the mod does nothing in game | The scripts were never exported, so the PBO holds the last export |
| The mod loads but a dependency does not | It is not in the mod chain, or it is in the chain after the mod that needs it |
| It works offline and breaks on a server | Read the offline list above, starting with the COT permissions line |
| A script came back with every line changed | Report it. That is a defect, not a setting |

## Next

- [examples/](examples/) has four graphs and the script each one generates.
- [node-reference.md](node-reference.md) is the palette on a page, with the
  cautions each node carries.
- [architecture.md](architecture.md) is how the parts fit together, if you want
  to change one.
