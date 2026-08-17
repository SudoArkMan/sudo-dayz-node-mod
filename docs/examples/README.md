# Worked examples

Four small graphs, each one the smallest thing that does something real, with
the Enforce Script it produces beside it.

All four live in [examples.sdzn](examples.sdzn) as separate scripts. Open that
file in the application and you can click through them, or read them here.

If you have not built a mod with this tool yet, start with
[getting-started.md](../getting-started.md) instead. It walks one graph from an
empty window to a line in a DayZ log, and the graph it builds is
[first-mod.sdzn](first-mod.sdzn) beside this file. The four below assume you
already know what an exec wire is.

**The script under each example is the tool's own output, not code written to
look like it.** It was produced by running the generator over `examples.sdzn`
and pasting what came back:

```
build/nodedoc/nodedoc --generate docs/examples/examples.sdzn
```

`nodedoc` is the small tool that also writes [the node
reference](../node-reference.md). Building it is in
[architecture.md](../architecture.md).

Every example reopens `ItemBase`, which is what an item's behaviour hangs off in
DayZ. The same shapes work on any class; only the event names change.

---

## 1. A timer that prints

**Goal.** Five seconds after the item exists, write a line to the log, and keep
doing it.

**Nodes.** Begin, Set Timer, Print, String literal.

**Wiring.**

| From | To |
| --- | --- |
| Begin `exec` | Set Timer `exec` |
| Set Timer `elapsed` | Print `exec` |
| String `ret` | Print `value` |

On the Set Timer node set `seconds` to `5.0`, turn `repeat` on, and in Details
name it `Beep`.

**What it generates.**

```c
class SUDO_TimerItem extends ItemBase
{
	ref Timer m_Beep;

	override void EEInit()
	{
		super.EEInit();
		m_Beep = new Timer(CALL_CATEGORY_SYSTEM);
		m_Beep.Run(5.0, this, "BeepElapsed", null, true);
	}

	void BeepElapsed()
	{
		Print("still here");
	}
};
```

**What to notice.** One node wrote four things that all have to agree: the
member, its construction, the string the engine dispatches on, and the method
that string has to resolve to. Naming the node `Beep` named all four.

The member is `ref`, and that is not decoration. The timer queue does not own
what you put in it, so without `ref` the `Timer` is collected the moment `EEInit`
returns and never fires. That is the classic version of "my timer does nothing".

`elapsed` is a separate exec pin from the node's own output, because the chain
on it runs later, in its own method. The chain leaving the plain `exec` pin
carries straight on inside `EEInit`.

**The neighbouring node.** Call Later is the same idea for the next frame or
250 ms rather than seconds. The pair is easy to get the wrong way round: Set
Timer takes seconds as a decimal, Call Later takes milliseconds as a whole
number. A deferred call also has to be cancelled by hand in your cleanup path,
where a Timer stops itself when its owner is destroyed.

---

## 2. An item that reacts to being attached

**Goal.** When something is attached to this item, on the server only, repair it
if it is an `ItemBase` and log which slot it went into.

**Nodes.** Event `EEItemAttached`, Server Only, Cast To (`ItemBase`), `SetHealth`,
Print.

**Wiring.**

| From | To |
| --- | --- |
| Event `exec` | Server Only `exec` |
| Server Only `exec` | Cast To `exec` |
| Event `item` | Cast To `object` |
| Cast To `success` | SetHealth `exec` |
| Cast To `as` | SetHealth `target` |
| SetHealth `exec` | Print `exec` |
| Event `slot_name` | Print `value` |

Set the Cast To node's class to `ItemBase` in Details, and type `100.0` into
SetHealth's `health` pin.

**What it generates.**

```c
class SUDO_ReactiveItem extends ItemBase
{
	override void EEItemAttached(EntityAI item, string slot_name)
	{
		super.EEItemAttached(item, slot_name);
		if (!GetGame().IsServer())
			return;
		ItemBase cast0;
		if (Class.CastTo(cast0, item))
		{
			cast0.SetHealth(100.0);
			Print(slot_name);
		}
	}
};
```

**What to notice.** The event's parameters come out of it as pins. `item` is an
`EntityAI` because that is what the engine passes, and `EntityAI` has no
`SetHealth` you can rely on for an item, so it goes through a cast first.

Cast To has a `success` pin and a `failed` pin because the cast really can fail:
somebody attaches a class you did not expect and the graph that assumed
otherwise is a null pointer at three in the morning. The `as` pin carries the
narrowed value, and it only exists inside the success branch.

`super` is called for you at the top of every event. You do not place a node for
it. Leaving `super` out of a reopened class is how one mod breaks every other
mod that touched the same class, so it is not left to memory.

---

## 3. A server-only guard

**Goal.** Do the authoritative setup once, on the server, and nowhere else.

**Nodes.** Begin, Server Only, Self, `SetHealth`, Print, String literal.

**Wiring.**

| From | To |
| --- | --- |
| Begin `exec` | Server Only `exec` |
| Server Only `exec` | SetHealth `exec` |
| Self `ret` | SetHealth `target` |
| SetHealth `exec` | Print `exec` |
| String `ret` | Print `value` |

Type `100.0` into SetHealth's `health` pin.

**What it generates.**

```c
class SUDO_GuardedItem extends ItemBase
{
	override void EEInit()
	{
		super.EEInit();
		if (!GetGame().IsServer())
			return;
		SetHealth(100.0);
		Print("server set this up");
	}
};
```

**What to notice.** `EEInit` runs on the client as well as the server. Every
lifecycle event does. Anything touching health, inventory or spawning belongs
behind this guard, or every client runs it too and the results disagree with the
server's.

Server Only is an early return, so everything downstream of it is server side.
When you want both sides to do something different, use a Branch with `IsServer`
wired into its condition instead: one branches, the other stops.

`#ifdef SERVER` is the third form of the same question, asked at compile time
rather than at runtime, and it decides whether the code exists at all rather
than whether it runs. There is no `CLIENT` define in DayZ; client-only code is
written as the absence of `SERVER`.

Self wired into a `target` pin generates nothing at the call site, because the
call is already on `this`. Wire something else in and the same node calls it on
that object instead.

---

## 4. Reading a config value

**Goal.** Read a number out of this item's own config class and keep it.

**Nodes.** Begin, Self, `ConfigGetFloat`, Set `m_Weight`, Print.

**Setup.** Declare a float variable in the Variable Manager first, named
`m_Weight`. Its Get and Set nodes come from there: they only exist in this
graph, because the variable does.

**Wiring.**

| From | To |
| --- | --- |
| Begin `exec` | ConfigGetFloat `exec` |
| Self `ret` | ConfigGetFloat `target` |
| ConfigGetFloat `return` | Set m_Weight `value` |
| ConfigGetFloat `exec` | Set m_Weight `exec` |
| Set m_Weight `exec` | Print `exec` |
| Set m_Weight `ret` | Print `value` |

Type `weight` into the `entryName` pin.

**What it generates.**

```c
class SUDO_ConfigReader extends ItemBase
{
	float m_Weight = 0;

	override void EEInit()
	{
		super.EEInit();
		m_Weight = ConfigGetFloat("weight");
		Print(m_Weight);
	}
};
```

**What to notice.** This is how a mod parameterises behaviour per class without
writing a script subclass for every variant: put the number in `config.cpp` and
read it back. It is 398 sites in DayZ Expansion across 89 files, so it is the
normal way to do this rather than a trick.

The Set node passes the value straight out again, so Print does not need a
second Get node beside it.

Two limits worth knowing before you build on this. **Nested arrays in
`config.cpp` cannot be read from script at all.** And `ConfigGetFloat` on the
object reads that object's own config class, while `ConfigGetText` on the game
takes a whole config path as a string, which is the pair to keep straight.

There is a `ConfigIsExisting` node in the same group. Reach for it when the
entry is optional, rather than reading the value and guessing from what comes
back.

---

## Where the generated file goes

Everything above shows the class the generator writes. The file it lands in also
carries the user region:

```c
	// >>> user code, kept when the graph regenerates
	// helpers you write here are preserved
	// <<< user code
```

Anything between those two markers is read back and put in again every time the
script is regenerated. It is where a helper method goes when it is easier to
write than to wire.

## Next

- [../node-reference.md](../node-reference.md) is every node family with the
  cautions each one carries.
- [../getting-started.md](../getting-started.md) covers exporting these to a
  mod folder, packing it and launching a test.
