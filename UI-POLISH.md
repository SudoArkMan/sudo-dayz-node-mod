# Interface polish, from reading the screenshots

The app grew feature by feature and every dock was sized when it was the new thing. This is the
list of what is actually wrong, taken from `app-start.png` and `app-editor.png` rather than from
opinion. `DESIGN.md` stays the binding contract; nothing here overrides it.

## The editor, worst first

**The left column is sized for the wrong panel.** `buildDocks` weights it
`{120, 170, 520, 110}` for outliner, palette, events, explorer. On a 1600x950 window that leaves the
Graph Outliner showing three rows with `GetHealth` clipped mid-word, the Node Palette showing two,
the Mod Explorer showing one line of prose, and the Events list holding more than half the column.
Events earned that share when it was the feature being built, not by use. The outliner is how you
move around a graph and it is the smallest thing on screen.

**The generated file is bigger than the canvas.** The bottom dock takes about 40% of the window
height, so the surface the whole app exists to show gets less room than a read-only view of its
output. `resizeDocks({codeDock}, {320})` plus `setMinimumHeight(180)` is the cause. The code view
wants enough lines to read a method, not a third of the window.

**The canvas does not frame its content.** In `app-editor.png` every node sits in the bottom right
with a large empty region up and to the left, at 75%. Opening a project should land the view on the
graph.

## The start page

**It is mostly void.** Three columns hold one recent project, three actions and four templates, and
the rest of a 1600x950 window is empty. The recent list also carries a bordered container that
stretches to the bottom of the page while Start and Templates have none, so one column reads as a
filled panel and the other two as floating stacks. Either all three get a container or none do.

**The status bar is the editor's.** `0 Errors`, `0 Warnings` and a `100%` zoom sit under the start
page, where there is no graph to have errors in and nothing to zoom.

## Wording and case

The splash art and the corner mark are supplied PNGs and their lettering is exempt. Everything Qt
draws is not: check every label, menu, tooltip and dialog for sentence case, and for the em-dash,
the middot and the ellipsis character, none of which may appear.

One mismatch to raise rather than fix: the splash artwork reads `BUILD 0.4.1` while the app reports
`v0.1.0` from `CMakeLists.txt`. Those are two different numbers on one screen and the art is the
user's to change.

## How to judge it

Screenshot at 1600x950 and at 1280x800, and read the PNG. A dock that clips a word mid-character, a
list showing fewer than four rows, or a panel holding more height than it has content for is the
defect. Sizes are only right when the window is small: at 1280x800 the left column has to give
every panel something usable rather than starving three to feed one.
