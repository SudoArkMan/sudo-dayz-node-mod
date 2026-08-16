# Node reference

This file is generated. Do not edit it: run
`nodedoc --reference docs/node-reference.md` after changing the node
tables, and commit what comes out. Building the generator is in
[architecture.md](architecture.md).

Every vanilla class, method, enum, global function and constant is a
node. The catalogue in this build holds 6108 classes and 29024 methods,
of which 2193 are events and 8392 are pure, plus 403 enums, 362
global functions and 578 constants. It is built from `P:\scripts`.

Searching for one of those by name is what the palette's search box
is for. This page is the other half: which nodes to reach for when
you do not already know the name of the thing you want.

## The groups

The palette is arranged by what you are trying to do, not by which
engine class a method happens to sit on. 13 groups are written by
hand and ordered by how often the thing they cover appears in shipped
mod code, measured in [notes/expansion.md](notes/expansion.md). The
last one is generated from whatever the other 13 do not name, so a
node added to the tool cannot go missing from the palette.

| Group | Nodes |
| --- | --- |
| Run this when something happens | 3 nodes |
| Do something later | 4 nodes |
| Decide what runs | 7 nodes |
| Server, client, or both | 5 nodes |
| Get the type you need | 4 nodes |
| Read a config value | 10 nodes |
| Tell the other side | 7 nodes |
| Work out a value | 5 nodes |
| Maths and comparisons | 13 nodes |
| Members | 1 node |
| Arrays and lists | 11 nodes |
| See what it is doing | 3 nodes |
| When a node is the wrong shape | 3 nodes |
| Everything else | 5 nodes |

### Run this when something happens

A mod is a set of hooks. Pick the moment first, then say what happens. Expansion carries 6,101 overrides against 731 reopened classes, so this is where nearly every graph starts.

| Node | Shape | What it does |
| --- | --- | --- |
| Browse events... | this script | The hooks a class can override, ranked, with the method each one becomes. Give this script a base class to fill the list. |
| Begin | runs once on init | Where a script starts. The first thing to wire on a new class. |
| End | runs once on destroy | Where a script cleans up. The entity is being destroyed. |

Watch out for:

- **Begin.** Runs on client and server. Guard anything authoritative with `GetGame().IsServer()`.
- **Begin.** On Construct is too early to touch attachments, config values or the inventory.
- **End.** Kill timers, effects and anything you registered here, or they outlive the entity.

### Do something later

529 sites in Expansion, and it is what you reach for when a value is not ready yet. A Timer stops itself when the item is deleted; a deferred call does not, and forgetting to cancel one is a shipped vanilla bug.

| Node | Shape | What it does |
| --- | --- | --- |
| Set Timer | after N seconds | Runs the chain on its `elapsed` pin after a delay. The flow carries straight on out of the exec pin; `elapsed` is a separate method that runs later. |
| Call Later | after N milliseconds | Defers the chain on its `then` pin to the call queue. The usual answer to a DayZ trap that only goes away if you wait a frame. |
| Stop Timer | Timer.Stop() | Stops a timer that a Set Timer node started. |
| Cancel Call Later | CallQueue.Remove() | Takes a deferred call back off the queue. |

Watch out for:

- **Set Timer.** The queue matters. System runs always; Gameplay stops while the player has the in-game menu open, which is wrong for anything authoritative. System is the default.
- **Set Timer.** Destroying the object stops the timer on its own, because releasing the `ref` runs the destructor and that takes it off the queue. Call Later is the one that needs cancelling by hand.
- **Call Later.** An entry stays in the queue until it is removed or it stops repeating. Not removing one in your cleanup path is a bug vanilla itself has shipped. Use Cancel Call Later.
- **Call Later.** Leaving the delay at 0 still defers to the next tick, which is the point when the fix is 'do this one frame later'.
- **Stop Timer.** A repeating timer runs until something stops it. This is that something.
- **Cancel Call Later.** The queue is part of the identity. Removing from the System queue does not cancel something scheduled on Gameplay.

### Decide what runs

Wire the condition, not the text. Note Call Super: inside a modded class Expansion calls super in about 80 percent of its overrides, against roughly 29 percent elsewhere, and leaving it out is how a mod breaks every other mod on the same class.

| Node | Shape | What it does |
| --- | --- | --- |
| Branch | if / else | Splits the flow in two based on a condition: an `if / else`. |
| For Each | iterate an array | Walks every element of an array. |
| For Loop | int counter | Counts from first to last, running the body each time. |
| While | loop while true | Repeats the body for as long as the condition holds. |
| Sequence | run in order | Runs several chains one after another from a single trigger. |
| Call Super | super.Event(...) | Calls the base class implementation. |
| Return | exit the function | Leaves the current event or function immediately. |

Watch out for:

- **For Each.** A null array will throw at runtime. Check it first if the source can return null.
- **For Loop.** `last` is exclusive, so use the array size, not size minus one.
- **While.** Nothing changes the condition for you, so make sure the body can end the loop.
- **Call Super.** Use the event's "skip super" option instead of placing this node.

### Server, client, or both

412 runtime checks against 269 compile-time guards, and they are not the same thing: one branches, the other decides whether the code exists at all. There is no CLIENT define in DayZ; client-only code is written as the absence of SERVER.

| Node | Shape | What it does |
| --- | --- | --- |
| Server Only | early-out on the client | Stops the flow here when running on a client. |
| IsServer | CGame | Declared on CGame. IsServer() : bool |
| IsClient | CGame | Declared on CGame. IsClient() : bool |
| IsMultiplayer | CGame | Declared on CGame. IsMultiplayer() : bool |
| IsDedicatedServer | CGame | Robust check which is preferred than the above, as it is valid much sooner [note] You may want to use #ifdef SERVER instead for slight performance... |

Watch out for:

- **Server Only.** Events like Begin fire on client and server. Anything touching health, inventory or spawning belongs behind this.
- **IsServer.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **IsClient.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **IsMultiplayer.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **IsDedicatedServer.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.

### Get the type you need

1,154 `Class.CastTo` calls, the single most repeated line in the corpus. Cast To has a success pin and a failed pin because the cast really can fail, and a graph that ignores that is the null pointer you get at 3AM.

| Node | Shape | What it does |
| --- | --- | --- |
| Cast To | Class.CastTo | Tries to treat an object as a more specific class. |
| Self | this | The instance the current script is running on: `this`. |
| New Object | new Class(), not entities | Creates a plain script object with `new`: helpers, data holders, Timers. |
| Spawn Entity | CreateObjectEx | Creates a real entity in the world. |

Watch out for:

- **Cast To.** This is how you safely narrow an `EntityAI` to an `ItemBase`. Never assume the type.
- **New Object.** Not for entities. Anything descending from `Object` (items, players, vehicles, buildings) has an engine object behind it that `new` cannot create. Use Spawn Entity.
- **New Object.** A `new` object held only in a local dies when the call ends. Store it in a class variable if it must survive, which is what a Timer needs.
- **Spawn Entity.** Server-side only. Put a Server Only node ahead of it, or clients will try to spawn their own copy.
- **Spawn Entity.** The class name is the config class from `CfgVehicles`, not the script class. The two are usually but not always the same.

### Read a config value

398 sites in 89 files. This is how a mod parameterises behaviour per class without writing a script subclass, and how CfgMods becomes a registry other mods can publish into. Nested arrays in config.cpp cannot be read from script at all.

| Node | Shape | What it does |
| --- | --- | --- |
| ConfigGetString | Object | config class API |
| ConfigGetInt | Object | Declared on Object. ConfigGetInt(string) : int |
| ConfigGetFloat | Object | Declared on Object. ConfigGetFloat(string) : float |
| ConfigGetBool | Object | Declared on Object. ConfigGetBool(string) : bool |
| ConfigGetTextArray | Object | proto ParamEntry ConfigGetEntry(string entryName); Get array of strings from config entry. param entryName; param value output |
| ConfigIsExisting | Object | Checks if given entry exists. param entryName |
| ConfigGetText | CGame | config functions Get string value from config on path. param path of value, classes are delimited by empty space. |
| ConfigIsExisting | CGame | Declared on CGame. ConfigIsExisting(string) : bool |
| ConfigGetChildrenCount | CGame | Get count of subclasses in config class on path. param path of value, classes are delimited by empty space. |
| ConfigGetChildName | CGame | Get name of subclass in config class on path. param path of value, classes are delimited by empty space. |

Watch out for:

- **ConfigGetTextArray.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **ConfigIsExisting.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **ConfigGetChildrenCount.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.

### Tell the other side

314 RPC registrations and 185 synced variables. A synced variable only reaches clients after SetSynchDirty, and an RPC id that collides with another mod fails silently rather than loudly.

| Node | Shape | What it does |
| --- | --- | --- |
| RegisterNetSyncVariableBool | EntityAI | @fn RegisterNetSyncVariableBool registers bool variable synchronized over network; param variableName \p which variable should be synchronized |
| RegisterNetSyncVariableInt | EntityAI | @fn RegisterNetSyncVariableInt registers int variable synchronized over network; param variableName \p which variable should be synchronized; param... |
| RegisterNetSyncVariableFloat | EntityAI | @fn RegisterNetSyncVariableFloat registers float variable synchronized over network; param variableName \p which variable should be synchronized;... |
| SetSynchDirty | EntityAI | Sets object synchronization dirty flag, which signalize that object wants to be synchronized (take effect only in MP on server side) |
| RPCSingleParam | Object | Remote procedure call shortcut, see CGame.RPCSingleParam / CGame.RPC |
| RPCSingleParam | CGame | Declared on CGame. RPCSingleParam(Object, int, Param, bool, PlayerIdentity) |
| RPC | CGame | Initiate remote procedure call. When called on client, RPC is evaluated on server; When called on server, RPC is executed on all clients; param target... |

Watch out for:

- **RegisterNetSyncVariableBool.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **RegisterNetSyncVariableInt.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **RegisterNetSyncVariableFloat.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **SetSynchDirty.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **RPCSingleParam.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.
- **RPC.** Engine-implemented (`proto native`). You can call it, but a `modded class` override will not compile.

### Work out a value

Pure nodes: no exec pins, they evaluate where they are used. The Enforce compiler mishandles `bool x = a && b;`, so the generator spills every intermediate into its own named local, which a wire already implies.

| Node | Shape | What it does |
| --- | --- | --- |
| Operator | a op b | Combines two values with an arithmetic, comparison or logical operator. |
| Not | !value | Inverts a true/false value. |
| Select | cond ? a : b | Picks one of two values based on a condition. |
| Literal | string | A fixed value of whatever type you choose. |
| Class Name | typename literal | A class name as a value (a `typename`). |

### Maths and comparisons

One node per operator, so placing a subtraction does not mean placing an Operator and then finding where its symbol is set. Each one names the value it yields, and a comparison always yields a bool whatever it is given.

| Node | Shape | What it does |
| --- | --- | --- |
| + | add | add (`a + b`). On strings this joins them, and a number joined to a string converts itself. |
| - | subtract | subtract (`a - b`). Outputs a bool. |
| * | multiply | multiply (`a * b`). Outputs a bool. |
| / | divide | divide (`a / b`). Dividing two ints truncates in Enforce, so make one of them a float when you want a fraction. |
| % | modulo, remainder | modulo, remainder (`a % b`). Integers only. |
| == | equals | equals (`a == b`). Outputs a bool. |
| != | not equal | not equal (`a != b`). Outputs a bool. |
| < | less than | less than (`a < b`). Outputs a bool. |
| <= | less or equal | less or equal (`a <= b`). Outputs a bool. |
| > | greater than | greater than (`a > b`). Outputs a bool. |
| >= | greater or equal | greater or equal (`a >= b`). Outputs a bool. |
| && | and | and (`a && b`). Both sides are bools, and the result is a bool. |
| \|\| | or | or (`a \|\| b`). Both sides are bools, and the result is a bool. |

### Members

Writing into something that already exists. Declare the member in the Variable Manager first; its Get and Set nodes come from there, because they only exist in this graph.

| Node | Shape | What it does |
| --- | --- | --- |
| Set Member | name = value | Assigns to a member this graph does not declare: one the base class owns, or one on another object. |

Watch out for:

- **Set Member.** The name is written out as typed. Nothing here checks that the base class really declares it.

### Arrays and lists

Everything an array can be asked. These take an array pin rather than an object pin, which is what lets them join to the array<ref Something> a call hands back. Make Array is the one that builds a new one, with plus and minus on the node for its elements.

| Node | Shape | What it does |
| --- | --- | --- |
| Make Array | a new array | Builds an array and fills it in. Use the plus and minus on the node to add and remove elements. |
| Array Count | Count() | How many elements an array holds. |
| Array Get | Get(index) | Reads one element by its index. |
| Set Element | array[index] = value | Writes one slot of an array by its index. |
| Array Insert | Insert(value) | Adds one element to the end of an array. |
| Array Insert At | InsertAt(value, index) | Adds one element at a position, moving the rest along. |
| Array Remove | Remove(index) | Takes one element out by its index. |
| Array Clear | Clear() | Empties an array without replacing it. |
| Array Find | Find(value) | The index of the first matching element. |
| Array Sort | Sort(reverse) | Sorts an array in place. |
| For Each Index | count and loop | Walks an array by index rather than by element. |

Watch out for:

- **Make Array.** It has exec pins because it writes a statement. Put it on the chain ahead of whatever reads the array.
- **Array Count.** Calling it on a null array throws. Arrays that come back from the engine can be null.
- **Array Get.** Indexes start at 0 and reading past the end throws. Check against Array Count first.
- **Set Element.** The slot has to exist already. Writing past the end of an array throws; use Insert to grow one.
- **Array Remove.** `Remove` moves the last element into the gap rather than shifting everything down, so the order changes. `RemoveOrdered` is the one that keeps it.
- **Array Remove.** Removing while a For Each is walking the same array is the classic way to skip an element. Count downwards with a For Loop instead.
- **Array Sort.** It sorts what the engine can compare: numbers and strings. An array of script objects has no order the engine knows about.
- **For Each Index.** Removing elements inside the body changes what Count answers part way through. Count downwards when the body removes.

### See what it is doing

Print truncates at 1026 characters on the server and 240 on the client, which are different undocumented limits. Long output belongs in PrintToRPT.

| Node | Shape | What it does |
| --- | --- | --- |
| Print | Print(value) | Writes a line to the script log. |
| PrintToRPT | global | Prints content of variable to RPT file (performance warning - each write means fflush! use with care) |
| Error | global | Messagebox with error message |

Watch out for:

- **Print.** Printing every frame floods the log and costs real performance on a live server.

### When a node is the wrong shape

The way out. Raw Enforce keeps a statement as text and still generates, and the importer refuses rather than half-recognising, so text here is a decision and not a failure.

| Node | Shape | What it does |
| --- | --- | --- |
| Raw Enforce | inline code | Drops hand-written Enforce Script straight into the flow. |
| Raw Expression | inline value | Hand-written Enforce used as a value. |
| Comment | sticky note | A note on the canvas. |

Watch out for:

- **Raw Enforce.** Reading is not compiling. A name that exists but is the wrong one, an argument of the wrong type, a missing semicolon: all of those still reach the generated file.
- **Raw Expression.** The value is still not type-checked against the pin it feeds.

### Everything else

Builtin nodes the groups above do not name. Nothing is left out of the palette, so a node added to the tool turns up here until it earns a place of its own.

| Node | Shape | What it does |
| --- | --- | --- |
| Bool | Literals | A fixed true/false value. |
| Int | Literals | A fixed whole number. |
| Float | Literals | A fixed decimal number. |
| String | Literals | A fixed piece of text. |
| Vector | Literals | A fixed position or direction. |

## The four ways a script starts

Enforce has no single begin-play. Begin carries the choice, set in
Details on the node, and becomes the method named here with `super`
called for you.

| Mode | Becomes | When it fires |
| --- | --- | --- |
| On Init: EEInit() | `EEInit()` | The entity exists and is set up. The usual choice, and the closest thing to Begin Play. |
| On Construct: constructor | the constructor | Earliest point, before the entity is initialised. Good for registering sync variables, wrong for anything that touches attachments or config. |
| Deferred Init: DeferredInit() | `DeferredInit()` | Runs a frame after init, once the world around the entity is settled. Use it when On Init turns out to be too early. |
| After Load: AfterStoreLoad() | `AfterStoreLoad()` | Runs once persisted values have been restored. Use it to re-apply state a saved item came back with. |

## Operators

One node per operator rather than one Operator node with a symbol to
set, so placing a subtraction is one action.

| Node | Emits | Yields |
| --- | --- | --- |
| + | `a + b` | the type of its inputs |
| - | `a - b` | the type of its inputs |
| * | `a * b` | the type of its inputs |
| / | `a / b` | the type of its inputs |
| % | `a % b` | the type of its inputs |
| == | `a == b` | bool |
| != | `a != b` | bool |
| < | `a < b` | bool |
| <= | `a <= b` | bool |
| > | `a > b` | bool |
| >= | `a >= b` | bool |
| && | `a && b` | bool |
| \|\| | `a \|\| b` | bool |

Literal nodes take one of these types: `bool`, `int`, `float`, `string`, `vector`, `typename`.

Timing nodes run on one of three queues, set in Details:

- System: runs always, including on a server
- Gameplay: pauses while the in-game menu is open
- GUI: client side, for interface work

## Every builtin node

The nodes that are not vanilla API: flow control, operators,
literals, arrays, timing, variable access and the way out to raw
Enforce. Everything else in the palette comes from the catalogue and
is documented by DayZ's own declarations.

### Lifecycle

#### Begin, runs once on init

Where a script starts. The first thing to wire on a new class.

Out: exec, self (auto)

- Pick the moment in Details; the node becomes that Enforce method with `super` called for you.
- Defaults to `EEInit()`, which fires once the entity exists and is set up.

Caution: Runs on client and server. Guard anything authoritative with `GetGame().IsServer()`.

Caution: On Construct is too early to touch attachments, config values or the inventory.

Node id `bi.begin`.

#### End, runs once on destroy

Where a script cleans up. The entity is being destroyed.

Out: exec, self (auto), parent (EntityAI)

- Becomes `override void EEDelete(EntityAI parent)`, with `super` called for you.
- The `parent` pin is whatever the entity was attached to, if anything.

Caution: Kill timers, effects and anything you registered here, or they outlive the entity.

Node id `bi.end`.

### Flow

#### Branch, if / else

Splits the flow in two based on a condition: an `if / else`.

In: exec, condition (bool)  
Out: true, false

- Emits `if (condition) { ... } else { ... }` around whatever each exec pin leads to.
- Leaving the false pin unwired emits a plain `if` with no `else`.

Node id `bi.branch`.

#### Sequence, run in order

Runs several chains one after another from a single trigger.

In: exec  
Out: then 0, then 1, then 2

- Each `then` pin runs to completion in order. Useful when one event drives unrelated work.

Node id `bi.sequence`.

#### For Loop, int counter

Counts from first to last, running the body each time.

In: exec, first (int), last (int)  
Out: body, index (int), done

- Emits `for (int i = first; i < last; i++)`.
- The `index` pin carries the counter into the body.

Caution: `last` is exclusive, so use the array size, not size minus one.

Node id `bi.forLoop`.

#### For Each, iterate an array

Walks every element of an array.

In: exec, array (array<any>)  
Out: body, item (any), index (int), done

- Emits `foreach (Type item : array)`.
- Wiring the `index` pin switches to the counted form, `foreach (int i, Type item : array)`.
- The element type is taken from whatever feeds the array pin.

Caution: A null array will throw at runtime. Check it first if the source can return null.

Node id `bi.forEach`.

#### While, loop while true

Repeats the body for as long as the condition holds.

In: exec, condition (bool)  
Out: body, done

- Emits `while (condition) { ... }`.

Caution: Nothing changes the condition for you, so make sure the body can end the loop.

Node id `bi.while`.

#### Return, exit the function

Leaves the current event or function immediately.

In: exec, value (any)  

- Emits `return;`, or `return value;` when the value pin is wired.

Node id `bi.return`.

#### Server Only, early-out on the client

Stops the flow here when running on a client.

In: exec  
Out: exec

- Emits `if (!GetGame().IsServer()) return;`.

Caution: Events like Begin fire on client and server. Anything touching health, inventory or spawning belongs behind this.

Node id `bi.serverOnly`.

#### Call Super, super.Event(...)

Calls the base class implementation.

In: exec  
Out: exec

- Redundant in most graphs: `super` is emitted at the top of every event already.

Caution: Use the event's "skip super" option instead of placing this node.

Node id `bi.super`.

### Variables

#### Self, this

The instance the current script is running on: `this`.

Out: self (auto)

Pure: no exec pins, it evaluates where it is used.

- Wire it into any `target` pin to act on the object itself.

Node id `bi.self`.

#### Set Element, array[index] = value

Writes one slot of an array by its index.

In: exec, array (array<any>), index (int), value (any)  
Out: exec

- Emits `array[index] = value;`.
- Reading a slot is the Get Element node, or an index typed into a Raw Expression.

Caution: The slot has to exist already. Writing past the end of an array throws; use Insert to grow one.

Node id `bi.setElement`.

#### Set Member, name = value

Assigns to a member this graph does not declare: one the base class owns, or one on another object.

In: exec, target (auto), value (any)  
Out: exec

- Set the name in Details. Emits `name = value;`, or `target.name = value;` when the target pin is wired.
- A member this script declares itself has its own Set node in the Variables panel, which is the one to prefer.

Caution: The name is written out as typed. Nothing here checks that the base class really declares it.

Node id `bi.setMember`.

### Operators

#### Operator, a op b

Combines two values with an arithmetic, comparison or logical operator.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

- Pick the operator in Details. Comparison and logical operators output a bool.
- Available operators: + - * / % == != < <= > >= && ||.
- `+` on strings concatenates, and a number joined to a string converts itself. That is how text is built in Enforce.

Node id `bi.op`.

#### +, add

add (`a + b`). On strings this joins them, and a number joined to a string converts itself.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.+`.

#### -, subtract

subtract (`a - b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.-`.

#### *, multiply

multiply (`a * b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.*`.

#### /, divide

divide (`a / b`). Dividing two ints truncates in Enforce, so make one of them a float when you want a fraction.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op./`.

#### %, modulo, remainder

modulo, remainder (`a % b`). Integers only.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.%`.

#### ==, equals

equals (`a == b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.==`.

#### !=, not equal

not equal (`a != b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.!=`.

#### <, less than

less than (`a < b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.<`.

#### <=, less or equal

less or equal (`a <= b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.<=`.

#### >, greater than

greater than (`a > b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.>`.

#### >=, greater or equal

greater or equal (`a >= b`). Outputs a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.>=`.

#### &&, and

and (`a && b`). Both sides are bools, and the result is a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.&&`.

#### ||, or

or (`a || b`). Both sides are bools, and the result is a bool.

In: a (any), b (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

Node id `bi.op.||`.

#### Not, !value

Inverts a true/false value.

In: value (bool)  
Out: ret (bool)

Pure: no exec pins, it evaluates where it is used.

- Emits `!(value)`.

Node id `bi.not`.

#### Select, cond ? a : b

Picks one of two values based on a condition.

In: condition (bool), true (any), false (any)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

- Emits `(condition ? a : b)` inline, with no exec pins.

Node id `bi.select`.

### Literals

#### Literal, string

A fixed value of whatever type you choose.

In: v (string)  
Out: ret (string)

Pure: no exec pins, it evaluates where it is used.

- Set the type in Details; both pins follow it.
- Type the value directly on the node. Strings are quoted and vectors written as `"x y z"` when the script is generated.

Node id `bi.literal`.

#### Bool

A fixed true/false value.

In: v (bool)  
Out: ret (bool)

Pure: no exec pins, it evaluates where it is used.

- Type the value directly on the node.

Node id `bi.litBool`.

#### Int

A fixed whole number.

In: v (int)  
Out: ret (int)

Pure: no exec pins, it evaluates where it is used.

- Type the value directly on the node.

Node id `bi.litInt`.

#### Float

A fixed decimal number.

In: v (float)  
Out: ret (float)

Pure: no exec pins, it evaluates where it is used.

- Type the value directly on the node.

Node id `bi.litFloat`.

#### String

A fixed piece of text.

In: v (string)  
Out: ret (string)

Pure: no exec pins, it evaluates where it is used.

- Quotes are added for you when the script is generated.

Node id `bi.litString`.

#### Vector

A fixed position or direction.

In: v (vector)  
Out: ret (vector)

Pure: no exec pins, it evaluates where it is used.

- Written as `"x y z"`, because DayZ vectors are space-separated strings in script.

Node id `bi.litVector`.

#### Class Name, typename literal

A class name as a value (a `typename`).

In: v (typename)  
Out: ret (typename)

Pure: no exec pins, it evaluates where it is used.

- What `AddAction` and similar calls expect: the type itself, not an instance.

Node id `bi.litClass`.

### Casting

#### Cast To, Class.CastTo

Tries to treat an object as a more specific class.

In: exec, object (Class)  
Out: success, failed, as (auto)

- Emits `Class.CastTo(...)` inside an `if`, so the success pin only runs when the cast worked.
- The `as` pin carries the typed result into the success branch.

Caution: This is how you safely narrow an `EntityAI` to an `ItemBase`. Never assume the type.

Node id `bi.cast`.

#### New Object, new Class(), not entities

Creates a plain script object with `new`: helpers, data holders, Timers.

In: exec  
Out: exec, object (auto)

- Set the class in Details. The instance comes out of the object pin.

Caution: Not for entities. Anything descending from `Object` (items, players, vehicles, buildings) has an engine object behind it that `new` cannot create. Use Spawn Entity.

Caution: A `new` object held only in a local dies when the call ends. Store it in a class variable if it must survive, which is what a Timer needs.

Node id `bi.new`.

#### Spawn Entity, CreateObjectEx

Creates a real entity in the world.

In: exec, class name (string), position (vector)  
Out: exec, entity (EntityAI)

- Emits `GetGame().CreateObjectEx(type, position, ECE_PLACE_ON_SURFACE)` and casts the result to `EntityAI`.
- Leaving position unwired spawns at this entity's own position.

Caution: Server-side only. Put a Server Only node ahead of it, or clients will try to spawn their own copy.

Caution: The class name is the config class from `CfgVehicles`, not the script class. The two are usually but not always the same.

Node id `bi.spawn`.

### Timing

#### Set Timer, run something after N seconds

Runs the chain on its `elapsed` pin after a delay. The flow carries straight on out of the exec pin; `elapsed` is a separate method that runs later.

In: exec, seconds (float), repeat (bool)  
Out: exec, elapsed

- Writes the member, the construction and the call for you: `ref Timer m_Reload;`, `m_Reload = new Timer(CALL_CATEGORY_SYSTEM);` and `m_Reload.Run(5.0, this, "ReloadElapsed", null, false);`, plus the `ReloadElapsed()` method that holds the chain.
- The delay is in seconds and is a decimal. Call Later takes milliseconds as a whole number, which is the pair that is easiest to get the wrong way round.
- Name it in Details to name the member and the method after it. An unnamed one is named after the node, so two of them never collide.
- The member is always written `ref`. Without it the Timer is collected the moment the call returns and never fires, because the timer queue does not own what you put in it.
- Turn `repeat` on and it fires every N seconds until something stops it. Use Stop Timer for that.

Caution: The queue matters. System runs always; Gameplay stops while the player has the in-game menu open, which is wrong for anything authoritative. System is the default.

Caution: Destroying the object stops the timer on its own, because releasing the `ref` runs the destructor and that takes it off the queue. Call Later is the one that needs cancelling by hand.

Node id `bi.setTimer`.

#### Stop Timer, Timer.Stop()

Stops a timer that a Set Timer node started.

In: exec  
Out: exec

- Name the same timer in Details. Emits `if (m_Reload) m_Reload.Stop();`, guarded because the timer does not exist until the Set Timer node has run.
- Stop resets the countdown. There is no Continue after it; start it again with the Set Timer node.

Caution: A repeating timer runs until something stops it. This is that something.

Node id `bi.stopTimer`.

#### Call Later, run something after N ms

Defers the chain on its `then` pin to the call queue. The usual answer to a DayZ trap that only goes away if you wait a frame.

In: exec, milliseconds (int), repeat (bool)  
Out: exec, then

- Emits `GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(RefreshHud, 250, false);` and writes the `RefreshHud()` method the chain goes into.
- The delay is in milliseconds and is a whole number. Set Timer takes seconds as a decimal.
- The engine takes a reference to the method, not its name. That is why the node writes both ends: the quoted spelling is a different call (`CallLaterByName`) and passing a name here does not convert.

Caution: An entry stays in the queue until it is removed or it stops repeating. Not removing one in your cleanup path is a bug vanilla itself has shipped. Use Cancel Call Later.

Caution: Leaving the delay at 0 still defers to the next tick, which is the point when the fix is 'do this one frame later'.

Node id `bi.callLater`.

#### Cancel Call Later, CallQueue.Remove()

Takes a deferred call back off the queue.

In: exec  
Out: exec

- Name the same call in Details, and use the same queue. Emits `GetGame().GetCallQueue(CALL_CATEGORY_SYSTEM).Remove(RefreshHud);`.
- Removing one that is not queued does nothing, so it is safe in a cleanup path that may run twice.

Caution: The queue is part of the identity. Removing from the System queue does not cancel something scheduled on Gameplay.

Node id `bi.cancelCallLater`.

### Arrays

#### Make Array, a new array

Builds an array and fills it in. Use the plus and minus on the node to add and remove elements.

In: exec, elements, 0 to 32 of them  
Out: exec, array (array<any>), elements, 0 to 32 of them

- Set the element type in Details. Until it is set the element pins take anything, and the type is worked out from what the array is wired into, then from what is wired into the elements, then from what is typed on them.
- Writes `array<string> arr0 = {"a", "b"};` when it declares the array, which is the form vanilla uses.
- Writes `m_Junk = new array<string>();` and one `Insert` per element when the array goes into a member that already exists. Enforce takes a brace list only after a type in a declaration, so there is no choice about which form goes where.
- The element count is stored on the node, so it survives a save and comes back with an undo.

Caution: It has exec pins because it writes a statement. Put it on the chain ahead of whatever reads the array.

Node id `bi.makeArray`.

#### Array Count, Count()

How many elements an array holds.

In: array (array<any>)  
Out: ret (int)

Pure: no exec pins, it evaluates where it is used.

- Emits `array.Count()`.
- This is the number a For Loop counts up to, and it is exclusive: the last valid index is one less.

Caution: Calling it on a null array throws. Arrays that come back from the engine can be null.

Node id `bi.arrayCount`.

#### Array Get, Get(index)

Reads one element by its index.

In: array (array<any>), index (int)  
Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

- Emits `array.Get(index)`.
- Set the element type in Details to type the output pin, so a wire out of it is checked against the real class.

Caution: Indexes start at 0 and reading past the end throws. Check against Array Count first.

Node id `bi.arrayGet`.

#### Array Insert, Insert(value)

Adds one element to the end of an array.

In: exec, array (array<any>), value (any)  
Out: exec

- Emits `array.Insert(value);`.
- This is how an array grows. Set Element writes a slot that already exists and throws past the end.

Node id `bi.arrayInsert`.

#### Array Insert At, InsertAt(value, index)

Adds one element at a position, moving the rest along.

In: exec, array (array<any>), value (any), index (int)  
Out: exec

- Emits `array.InsertAt(value, index);`. The value comes first, which is the opposite way round from Set Element.

Node id `bi.arrayInsertAt`.

#### Array Remove, Remove(index)

Takes one element out by its index.

In: exec, array (array<any>), index (int)  
Out: exec

- Emits `array.Remove(index);`.

Caution: `Remove` moves the last element into the gap rather than shifting everything down, so the order changes. `RemoveOrdered` is the one that keeps it.

Caution: Removing while a For Each is walking the same array is the classic way to skip an element. Count downwards with a For Loop instead.

Node id `bi.arrayRemove`.

#### Array Clear, Clear()

Empties an array without replacing it.

In: exec, array (array<any>)  
Out: exec

- Emits `array.Clear();`.
- Anything else holding the same array sees it emptied too, which is the difference between this and building a new one.

Node id `bi.arrayClear`.

#### Array Find, Find(value)

The index of the first matching element.

In: array (array<any>), value (any)  
Out: ret (int)

Pure: no exec pins, it evaluates where it is used.

- Emits `array.Find(value)`.
- Answers -1 when nothing matches, so test for that rather than for 0, which is a real index.

Node id `bi.arrayFind`.

#### Array Sort, Sort(reverse)

Sorts an array in place.

In: exec, array (array<any>), reverse (bool)  
Out: exec

- Emits `array.Sort(reverse);`. The array itself changes; nothing comes back out.

Caution: It sorts what the engine can compare: numbers and strings. An array of script objects has no order the engine knows about.

Node id `bi.arraySort`.

#### For Each Index, count and loop

Walks an array by index rather than by element.

In: exec, array (array<any>)  
Out: body, index (int), item (any), done

- Emits `for (int i = 0; i < arr.Count(); i++)`, with the array held in a local first so it is not fetched every time round.
- The `item` pin reads `arr.Get(i)`, so both the position and the element are available inside the body.
- For Each is the one to reach for when only the element matters. This is the one that gives you the index as well.

Caution: Removing elements inside the body changes what Count answers part way through. Count downwards when the body removes.

Node id `bi.arrayForIndex`.

### Utility

#### Print, Print(value)

Writes a line to the script log.

In: exec, value (any)  
Out: exec

- Emits `Print(value);`. Anything can be wired in, and Enforce converts it for you.
- Output lands in the client or server script log, whichever side ran the node.

Caution: Printing every frame floods the log and costs real performance on a live server.

Node id `bi.print`.

#### Raw Enforce, inline code

Drops hand-written Enforce Script straight into the flow.

In: exec  
Out: exec

- The text is emitted verbatim at this point in the chain, indented to match.
- The code is read as Enforce, not treated as opaque text. Unbalanced braces or parentheses, a string or block comment left open, and names that nothing in this graph or the catalogue declares are all reported on the node.
- The node shows the code itself on the canvas, so a chain of these reads as the script it generates.

Caution: Reading is not compiling. A name that exists but is the wrong one, an argument of the wrong type, a missing semicolon: all of those still reach the generated file.

Node id `bi.raw`.

#### Raw Expression, inline value

Hand-written Enforce used as a value.

Out: ret (any)

Pure: no exec pins, it evaluates where it is used.

- Inlined verbatim wherever the output pin is wired.
- Balance and unknown names are checked the same way as a raw statement block, and the expression is shown on the node.

Caution: The value is still not type-checked against the pin it feeds.

Node id `bi.rawExpr`.

#### Comment, sticky note

A note on the canvas.

Pure: no exec pins, it evaluates where it is used.

- Purely for humans. It generates no script, and nothing in it is read as code.

Node id `bi.comment`.

