# SUDO DayZ Node Mod: UI contract

Visual scripting for DayZ Enforce Script, in Qt Widgets. Modelled on O3DE's
Script Canvas: dockable panels around a tabbed node canvas, dark chrome, no
decoration that does not carry information.

Reference implementation for all model/codegen behaviour is the Electron build
at `C:\Users\dilla\SUDO DayZ Script Node\src`. Port the semantics faithfully,
do not redesign them. The `.sdzn` project format must stay compatible in both
directions.

## Window layout

```
┌ menu: File Edit View Tools Preferences ──────────────────────────────────┐
├ toolbar: align/distribute, straighten wires, validate, generate ─────────┤
│ ┌ Graph Outliner ─┐┌ tab bar: one tab per script ──────┐┌ Variable Mgr ─┐│
│ │ filter box      ││                                    ││ Create Var ▾  ││
│ │ node list       ││          NODE CANVAS               ││ name type dflt││
│ ├ Node Palette ───┤│    (grid, nodes, bezier wires,     │├ Node Inspector┤│
│ │ search box      ││     sticky notes)                  ││ title + kind  ││
│ │ category tree   ││                                    ││ effects       ││
│ │                 ││                                    │├ MiniMap ──────┤│
│ └─────────────────┘└────────────────────────────────────┘└───────────────┘│
├ status: ● N Errors   ▲ N Warnings ─────────────────── zoom ──────────────┤
└──────────────────────────────────────────────────────────────────────────┘
```

Left dock column: Graph Outliner over Node Palette (split vertically).
Right dock column: Variable Manager, Node Inspector, MiniMap (top to bottom).
Bottom dock, spanning the canvas: Generated Code, live.
Docks are movable and closable; View menu restores them.

## Generated Code dock

The graph is the source but the .c file is what ships, so the generated script
sits under the canvas and updates as the graph changes (debounced, and skipped
entirely while the dock is hidden). Read-only, highlighted by the same
`EnforceHighlighter` the editor uses.

Every line records the node that produced it (`GenResult::lineOwners`), which
makes the view navigable in both directions: putting the cursor on a line
selects that node on the canvas, and selecting a node scrolls its lines into
view and tints them. Lines belonging to no single node (class header, member
declarations, the user region) carry an empty owner.

## Enforce language support

`src/enforce/lexer.*` tokenises Enforce: keywords and types checked against
`P:\scripts`, block comments carried across lines, preprocessor, strings with
escapes, hex and exponent numbers. `scanEnforce()` reports the identifiers,
calls, members and assignment targets in a block plus its brace and paren
balance, which is how the analyser checks Raw nodes instead of trusting them.
`enforceSummary()` gives a Raw node its header title.

Code-carrying nodes (`bi.raw`, `bi.rawExpr`, `bi.comment`) render their code on
the canvas in highlighted monospace, sized to content, rather than showing an
anonymous "Raw Enforce" box. Double-click opens the editor.

## Theme

Defined once in `theme.h/cpp`; canvas paints with these directly, widgets get
them via stylesheet. Near-black chrome, never pure black or pure white.

| Role | Colour |
| --- | --- |
| window / frame | `#1b1e23` |
| panel body | `#232830` |
| header / title bar | `#2b313b` |
| canvas | `#1a1d22` |
| grid minor / major | `#22262d` / `#2a2f37` |
| border | `#3a414c` |
| text / dim | `#d5dce4` / `#8b96a3` |
| accent (selection, focus) | `#3f7fb5` |
| error / warning | `#d9534f` / `#e0a53f` |

Node header accents come from `accents::` in `catalog.h` and must not be
re-invented: event `#7a2318`, call `#1d3a52`, pure `#234a2e`, flow `#2f353f`,
variable `#4a2c5e`, literal `#3c4022`, cast `#5e4a1d`, comment `#242c36`.

Pin colours come from `pinColor()` in `pins.h`.

Sentence case everywhere. No all-caps labels, no wide letter-spacing.

## Node visual spec

- Width 168 scene units; header 20; each pin row 13; corner radius 3.
- Header: accent fill, title in 8pt semibold, subtitle (owning class) dim and
  right-aligned, elided when long.
- Body `#262b33`; alternating row shade `#2b313a` behind inline editors.
- Pins: 3.4-unit circles, filled when connected, hollow when not. Exec pins
  are triangles, not circles, and always sit on the first row.
- Inputs on the left edge, outputs on the right, label inset 6.
- Unconnected literal inputs draw an inline editor: a rounded field showing
  the value (checkbox for bool, text otherwise).
- Selected: 1.5px `#3f7fb5` outline. Error: 1.5px `#d9534f`. Warning amber.
- A node with diagnostics shows a small badge in the header's right corner.

## Wires

Cubic bezier with horizontal tangents; control offset is
`clamp(|dx| * 0.5, 20, 120)`. Coloured by the source pin's type. Exec wires
2.2px, data wires 1.6px. Preview (dragging) wires are dashed. Wires draw
behind nodes (z = -1), sticky notes behind wires (z = -2).

## Interaction

| Action | Result |
| --- | --- |
| Left-drag empty canvas | marquee select |
| Middle-drag / space-drag | pan |
| Wheel | zoom about cursor, 0.25-3.0 |
| Drag pin -> pin | connect if `canConnect` and class-compatible |
| Drag pin -> empty | cancel |
| Right-click canvas | add-node search at that position |
| Double-click palette row | add node at view centre |
| Delete | remove selected nodes and their edges |
| Ctrl+D | duplicate selection, offset by 20 |
| Ctrl+Z / Ctrl+Shift+Z | undo / redo (graph snapshots in Document) |
| F | frame selection, Ctrl+F fit whole graph |

Every graph mutation goes through `Document::beginEdit(label)` /
`commitEdit()` so undo is uniform and panels refresh from one signal.

## Module contracts

Headers in `src/` are the contract. Implement against them, do not change a
signature without saying so.

- `pins.*`: done. Type parsing, colours, defaults, inline editor choice.
- `graph.*`: done. Model, connection rules, exec-chain walk, `.sdzn` JSON.
- `catalog.*`: done. Packed catalogue, lazy defs, search, `explain()`.
- `project.*`: done. `.sdzn` load/save, unknown fields preserved.
- `builtins.*`: flow/operator/literal/cast/variable/raw nodes + Begin modes.
- `codegen.*`: graph -> Enforce, user regions preserved between markers.
- `analysis.*`: correctness, DayZ traps, dead code -> diagnostics.
- `document.*`: session state, selection, undo, signals.
- `theme.*`: palette + stylesheet.
- `canvas/*`: scene, node/wire/note items, view, minimap.
- `panels/*`: palette, outliner, variables, inspector.
- `mainwindow.*`: menus, toolbar, tabs, docks, status bar.

## Verification

`DAYZSUDONodeMod --screenshot out.png resources/SUDO_Link.sdzn` renders the
window offscreen and exits, so the UI can be checked without a human at the
keyboard. Build with the Qt 6.11 MinGW kit already configured in `build/`.
