# DayZ Expansion, read against the files

Research behind several decisions in this project, kept because the measurements are the reason those decisions are what they are. The palette's group ordering in `src/nodeindex.cpp` comes from the counts below, and so do a number of the cautions written on individual nodes.

DayZ Expansion (experimental branch) is the largest DayZ mod there is: **1,963 `.c` files, 337,448 lines of Enforce Script, 349 `config.cpp` files, 233 `.layout` files, 25 preload addons.** Every claim below was checked against a working copy of the `DayZ-Expansion-Scripts` experimental branch, unpacked locally, and every path is relative to the root of that tree. Vanilla cross-checks are against `P:/scripts` and `P:/gui`. Where a count is given, it was recounted here rather than inherited.

None of Expansion's own source is reproduced here. What is quoted is a path, a line number and a count.

---

## The five things that change how you work

**1. It is 345 addons, not one mod.** Each folder holding a `config.cpp` becomes its own PBO with its own `CfgPatches` name and its own dependency edges. Script lives in only 54 of them. There are 421 `requiredAddons` edges, mean 1.22 per entry, 57 entries with none, **zero cycles**, four levels deep internally. The name you depend on is not the name you think: `DayZExpansion_Core` is a 14-line stub (`DayZExpansion/Core/Stub/config.cpp`) that owns no files and exists only so three GUI addons can write one dependency instead of two.

**2. Load order is filename sort, and 5,701 conditional compiles hang off it.** Walking all 345 `CfgPatches` entries produces **zero `requiredAddons` edges pointing at any `*_Preload` addon**. Nothing tells the engine that `DayZExpansion_Core_Preload` must precede `DayZExpansion_Core_Scripts`. The `0_` prefix sorting before `D` is the entire guarantee, and 5,701 `#ifdef`/`#ifndef` sites across 859 files depend on it resolving correctly. Rename a preload folder and 251 `#ifdef EXPANSIONMODAI` blocks quietly stop compiling, with no error.

**3. `modded class` is the exception, not the default.** 731 `modded class` sites against 2,249 plain `class` declarations. For every class Expansion reopens, it declares roughly three of its own. Reopening is what you do when the *engine*, not you, decides when the object is constructed. Everything else is a subclass you instantiate yourself.

**4. Every cross-boundary name is a string that nothing checks.** Widget names, RPC names, controller class names, `CfgMods` keys, stringtable keys, config properties, FSM state class names. A typo is a null pointer at runtime, not a compile error. This one corpus contains a duplicate `CfgMods` class name, two module flags referenced nine times and never defined, three uses of a misspelled widget style, four dangling stringtable keys, 40 orphaned layouts, four case-mismatched script folder declarations and 497 declared-but-absent script folders. None of them produce an engine error.

**5. The Enforce compiler is a hazard you write around, not a tool you trust.** Expansion cites 16 Bohemia tickets at 49 sites in 37 files, and one of them (T173348, wrong value from `bool x = <compound expression>;`) is cited 32 times in 24 files and has changed how the codebase writes every boolean. The avoidance idiom appears at least 101 times in 57 files.

---

## How the code gets compiled

### The addon graph

345 `CfgPatches` entries across 349 configs. The internal spine:

| Level | Addon | Requires |
|---|---|---|
| external | `JM_CF_Scripts` | Community Framework |
| external | `DF_Scripts` | Dabs Framework |
| 1 | `DayZExpansion_ModStorage_Scripts` | `JM_CF_Scripts` |
| 2 | `DayZExpansion_Core_Scripts` | `DayZExpansion_ModStorage_Scripts`, `DF_Scripts` |
| 3 | 21 subsystems | `DayZExpansion_Core_Scripts` plus vanilla data addons |
| 4 | Hardline, Quests, NamalskAdventure, VirtualContainerStorage | via VanillaFixes / AI / Vehicles / BaseBuilding |

`DayZExpansion/ModStorage/Scripts/config.cpp` is the root of the script tree and contains **zero `.c` files**. It declares five script modules pointing at five folders that do not exist. Its only job is to sit between Community Framework and Expansion Core so CF is guaranteed to compile first.

In-degree, internal:

| In-degree | Addon | Config |
|---:|---|---|
| 27 | `DayZExpansion_Core_Scripts` | `DayZExpansion/Core/Scripts/config.cpp` |
| 23 | `DayZExpansion_Vehicles_Data` | `DayZExpansion/Vehicles/Data/config.cpp` |
| 10 | `DayZExpansion_Core_GUI` | `DayZExpansion/Core/GUI/config.cpp` |
| 4 | `DayZExpansion_Dta_Core` | `DayZExpansion/Dta/Core/config.cpp` |
| 3 | `DayZExpansion_Core` | `DayZExpansion/Core/Stub/config.cpp` |

The 345 entries reference 46 addon names that are not in the tree, led by `DZ_Data` (109 edges), `DZ_Sounds_Effects` (32), `DZ_Weapons_Firearms` (17), `DZ_Characters` (14), `DZ_Structures_Signs` (11). The tail includes third-party mods hard-coded from NamalskAdventure: `ns3`, `Windstrideclothing`, `SkullMask`, `ns_dayz_gear_lehs`, `ns_dayz_gear_head`. DayZ warns on a missing `requiredAddons` target rather than refusing to start, which is why naming another author's clothing mod in `requiredAddons` is survivable.

There is not a single `$PBOPREFIX$` file in the corpus (searched case-insensitively across all 349 addon folders). The prefix is set by the build pipeline, and the repo-relative folder path is the prefix: every `files[]` path in every `CfgMods` block equals the folder's path from the repo root.

### Which layer a file belongs in, and why it is not a choice

Five script modules compile in order: `1_Core`, `2_GameLib`, `3_Game`, `4_World`, `5_Mission`. Where Expansion's `.c` files land:

| Layer | Files | `modded class` sites |
|---|---:|---:|
| `4_World` | 1,260 | 550 |
| `3_Game` | 386 | 67 |
| `5_Mission` | 282 | 111 |
| `Common` | 22 | 0 |
| `1_Core` | 13 | 3 |
| `2_GameLib` | **0** | **0** |

The layer is decided by where the vanilla class lives, full stop. `modded class Math` sits in `DayZExpansion/AI/Scripts/1_Core/DayZExpansion_AI/proto/enmath.c` because `Math` is in `P:/scripts/1_core/proto/enmath.c`, and the mod folder even reproduces `proto/`. Preload override files mirror the vanilla source path exactly: `0_DayZExpansion_AI_Preload/4_world/classes/playermodifiers/modifiersmanager.c` against `P:/scripts/4_world/classes/playermodifiers/modifiersmanager.c`. That is the house convention.

The one deliberate exception is reach. Symbols declared in `1_Core` are visible from every later module but not the reverse, so anything called from all four ends up there. `DayZExpansion/Core/Scripts/1_Core/DayZExpansion_Core/ExpansionString.c` is 625 lines of string helpers in the engine module, not in `3_Game` where you would expect utility code, because it has to serve `3_Game`, `4_World` and `5_Mission` alike. Only three subsystems put anything in `1_Core`, 13 files total.

**`2_GameLib` is a fiction.** It is declared 68 times across the configs and populated zero times. Every `gameLibScriptModule` block in the corpus points at nothing.

The wider picture, and the reason the copy-paste config template works at all:

| Layer folder | Declared in configs | Resolving on disk |
|---|---:|---:|
| `Common` | 336 | 110 |
| `1_Core` | 68 | 3 |
| `2_GameLib` | 68 | 0 |
| `3_Game` | 71 | 26 |
| `4_World` | 72 | 28 |
| `5_Mission` | 71 | 22 |
| **total** | **686** | **189** |

**497 of 686 declared script folders (72 percent) are not on disk.** The engine skips them silently. That tolerance is what makes the template viable, and it is also what hides typos. Four declarations resolve only because PBO lookup is case-insensitive: `0_DayZExpansion_AI_Preload/4_World` and `/5_Mission` (on disk `4_world`, `5_mission`), `0_DayZExpansion_Core_Preload/3_Game` (`3_game`), and `DayZExpansion/Vehicles/Scripts/1_Core` (`1_core`).

`DayZExpansion/DefineTest/config.cpp` is 66 lines and the only file in its addon. It registers five script modules against five folders that ship empty, and nothing in 1,963 `.c` files references it. It is a scratch slot: drop one `.c` file into any stage and see what a define resolves to there, without building an addon.

### The 25 preload addons

Each is a `config.cpp` with empty `requiredAddons` plus, usually, one `Common/DayZExpansion_*_Defines.c`. They exist because **Enforce resolves symbols across a whole script module regardless of file order, but `#define` visibility is strictly file-order dependent.** A define emitted from a subsystem's own folder is invisible to any sibling subsystem that compiled earlier.

The proof is in the tree. `#define EXPANSIONMODGROUPS` exists in exactly one place, `0_DayZExpansion_Groups_Preload/Common/DayZExpansion_Groups_Defines.c:14`. `DayZExpansion/Book/Scripts/5_Mission/DayZExpansion_Book/Book/BookTabs/Party/ExpansionBookMenuTabParty.c` wraps its entire class declaration in `#ifdef EXPANSIONMODGROUPS`. `DayZExpansion_Book_Scripts` and `DayZExpansion_Groups_Scripts` both require only `DayZExpansion_Core_Scripts`, so they are siblings and Book sorts first. Without the preload the define would arrive after Book compiled and every Party tab would vanish.

The second half of the trick is in the config. Every preload registers the same `Common` folder into **all five** script modules (`0_DayZExpansion_Core_Preload/config.cpp`), so the defines file compiles once into each of the five preprocessor namespaces. That is the only way a `#define` is visible in `1_Core` and `5_Mission` alike. 336 `Common` registrations exist across the corpus.

Consumers of the cross-module module flags, recounted:

| Define | `#ifdef`/`#ifndef` sites | Files | Defined at |
|---|---:|---:|---|
| `EXPANSIONMODAI` | 251 | 91 | `0_DayZExpansion_AI_Preload/Common/DayZExpansion_AI_Defines.c:3` |
| `EXPANSIONMODGROUPS` | 144 | 53 | `0_DayZExpansion_Groups_Preload/Common/DayZExpansion_Groups_Defines.c:14` |
| `EXPANSIONMODHARDLINE` | 143 | 52 | `DayZExpansion/Hardline/Scripts/config.cpp:40` via `defines[]` |
| `EXPANSIONMODNAVIGATION` | 106 | 29 | `0_DayZExpansion_Navigation_Preload/Common/DayZExpansion_Navigation_Defines.c:14` |
| `EXPANSIONMODBASEBUILDING` | 72 | 38 | `0_DayZExpansion_BaseBuilding_Preload/Common/DayZExpansion_BaseBuilding_Defines.c:16` |
| `EXPANSION_MODSTORAGE` | 66 | 66 | `0_DayZExpansion_ModStorage_Preload/config.cpp:24` via `defines[]` |
| `EXPANSIONMODMARKET` | 65 | 27 | `0_DayZExpansion_Market_Preload/Common/DayZExpansion_Market_Defines.c:14` |
| `EXPANSIONMODVEHICLE` | 58 | 30 | `0_DayZExpansion_Vehicles_Preload/Common/DayZExpansion_Vehicles_Defines.c:13` |
| `EXPANSIONMODWEAPONS` | 50 | 21 | `0_DayZExpansion_Weapons_Preload/Common/DayZExpansion_Weapons_Defines.c:14` |
| `EXPANSIONMODQUESTS` | 34 | 17 | `DayZExpansion/Quests/Scripts/config.cpp:42` via `defines[]` |

Four preloads are a `config.cpp` and nothing else: Garage, Killfeed, ModStorage, P2PMarket. ModStorage earns its keep through `defines[]`. The other three are reserved slots, and they still do work at runtime, because a `CfgMods` class is queryable from script:

```
if (g_Game.ConfigIsExisting("CfgMods DZ_Expansion_Animations_Preload"))
    warningText += " Some player animations MAY not work correctly.";
```
`DayZExpansion/Core/Scripts/5_Mission/DayZExpansion_Core/Mission/MissionServer.c:183`

An empty preload PBO is a capability beacon that any mod can test for, at compile time via its define and at runtime via `ConfigIsExisting`.

### Two mechanisms for publishing a define, and why both survive

DayZ also supports `defines[]` directly on a `CfgMods` class. Seven configs declare it (one of them empty: `DayZExpansion/NamalskAdventure/Scripts/config.cpp:43`), so six actually export flags. It is applied globally before any module compiles, not in mod load order. Proof: `EXPANSIONAI_ONFACTIONCHANGE` is declared at `DayZExpansion/AI/Scripts/config.cpp:43` and consumed at `DayZExpansion/Hardline/Scripts/4_World/DayZExpansion_Hardline/Entities/ManBase/PlayerBase.c:186`, and Hardline does not require AI.

So why keep 25 preload PBOs? Because `defines[]` is a flat list of unconditional strings and cannot express a conditional. All 78 `#define` sites in the corpus are bare flags with no replacement body and **zero take arguments** (the same is true of vanilla), but the *placement* of a flag can be conditional, and that has to live in a `.c` file compiled early:

```
0_DayZExpansion_Core_Preload/Common/DayZExpansion_Core_Defines.c:48
    #ifndef DEVELOPER
    #define EXTRACE
    #endif

    :87
    #ifdef SERVER //! Don't comment these, diag exe server crashes
    #define EXPANSION_DEBUG_SHAPES_DISABLE
    #endif

    :91
    #ifdef DabsFramework
    #define EXPANSIONUI
    #else
    #ifdef WORKBENCH
    #define EXPANSIONUI
    #endif
    #endif
```

`EXPANSIONUI` has 38 consumers in 28 files and is derived from whether a third-party mod is loaded. `EXTRACE` has 1,194 consumers in 179 files and is derived from `#ifndef DEVELOPER`. No `defines[]` array can produce either. There is even a deferred tier for defines that depend on other mods' defines, at `DayZExpansion/AI/Scripts/Common/DayZExpansion_AI_DeferredDefines.c:1-7`.

The same file publishes a compile-time API for other people. `0_DayZExpansion_Core_Preload/Common/DayZExpansion_Core_Defines.c:16-31` declares 16 `EXPANSION_GEQ_*` version flags and `:100` declares `EXPANSION_1_9_48` (written by `CI.bat` at build time, per the comment at `:99`). None of them is consumed anywhere in the 1,963 files. `EXPANSIONMODCORE` at `:77` carries the comment "Used for third party mods, if they want to know if expansion is loaded" and has exactly two internal consumers, both in MapAssets.

### Code in a preload

11 of the 25 preloads carry executable script, 14 `modded class` declarations total, and every one is a hook that has to beat other mods to the class:

| File | Classes reopened |
|---|---|
| `0_DayZExpansion_Core_Preload/3_game/dayzgame.c:1` | `DayZGame` |
| `0_DayZExpansion_Core_Preload/4_World/Entities/ItemBase.c:13` | `ItemBase` |
| `0_DayZExpansion_Core_Preload/4_World/Entities/ItemBase/Clothing.c:13` | `Clothing` |
| `0_DayZExpansion_Core_Preload/4_World/Entities/ManBase/PlayerBase.c:1` | `PlayerBase` |
| `0_DayZExpansion_AI_Preload/4_world/classes/playermodifiers/modifiersmanager.c:1` | `ModifiersManager` |
| `0_DayZExpansion_AI_Preload/4_world/entities/weapons/firearms/fsm/states/weaponfire.c:1,27,55,102` | four `WeaponFire*` classes |
| `0_DayZExpansion_AI_Preload/5_mission/missionserver.c:1` | `MissionServer` |
| `0_DayZExpansion_BaseBuilding_Preload/4_World/Classes/Hologram.c:16` | `Hologram` |
| `0_DayZExpansion_SpawnSelection_Preload/3_Game/Analytics/AnalyticsManagerServer.c:13` | `AnalyticsManagerServer` |
| `0_DayZExpansion_Vehicles_Preload/4_World/.../ActionStartEngine.c:13`, `ActionStopEngine.c:13` | two actions |

Code in a preload is the innermost modded layer, closest to vanilla, so every later mod that reopens the same class wraps around it. That is deliberate for `Hologram`, where Expansion forces the raycast back to the pre-1.16 `ObjIntersectView` (`0_DayZExpansion_BaseBuilding_Preload/4_World/Classes/Hologram.c:55-57`, repeated at `:82-84`) and notes it "breaks some 3rd party mods including our basebuilding". Expansion is restoring a default that anybody downstream can still take over.

The preload also proves its own ordering. `0_DayZExpansion_Core_Preload/3_game/dayzgame.c:30` declares `void Expansion_OnUpdate(bool, float)` with an empty body; `DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/DayZGame.c:256` overrides it. You cannot override a method that has not been declared yet.

---

## Reopening a class

### The rule

731 `modded class` sites in 666 files across 448 distinct classes, against 2,249 plain `class` declarations. Expansion reopens a class when the engine, not Expansion, decides when to construct it: `PlayerBase`, `ItemBase`, `CarScript`, `MissionGameplay`, `DayZGame`, `ActionConstructor`. It extends when it controls construction.

The most-reopened classes, by number of separate files that reopen them:

```
22  PlayerBase             9  IngameHud
20  ExpansionSettings      9  CarScript
16  ActionConstructor      8  DayZExpansion
15  ItemBase               7  ZombieBase / MissionBaseWorld / DayZPlayerImplement
12  MissionGameplay        5  PluginRecipesManager / ParticleList / DayZGame / AnimalBase
 9  ModItemRegisterCallbacks
```

The `super` chain order across those 22 `PlayerBase` layers is decided by the `requiredAddons` graph plus PBO filename sort, and nothing in the source states it. Because the compiler flattens all `modded class` bodies for a type into one class, the Market's `PlayerBase` additions can sit in the Market addon and the Quest's in the Quest addon, and shipping only one of them still compiles. **That single language feature is what makes the fifteen-mod split possible.**

### Namespacing, and where it stops

Expansion prefixes roughly 46 percent of the members it adds to a reopened class (`Expansion_`, `eAI_`, `CF_`). The unprefixed remainder is where a collision with another mod would land. `IngameHud` is the stress test: it is reopened from nine files across Core, AI, Groups, NameTags, Navigation, VanillaFixes, Vehicles and the root Scripts module, and the only thing preventing a collision is discipline. `DayZExpansion/Scripts/5_Mission/DayZExpansion/GUI/Hud/IngameHud.c:14-47` is 34 consecutively prefixed fields.

The corpus-wide totals are 1,154 `Class.CastTo` occurrences, 6,101 `override` occurrences, 3,448 `auto x =` sites and 1,163 `foreach` sites. And `GetGame()` is called **zero** times in 337,448 lines; `g_Game` appears on 2,971 lines. Vanilla defines `GetGame()` as a plain script function (`P:/scripts/3_game/gameplay.c:636`) and has largely migrated away from it too.

### One hook instead of twelve overrides

Twelve subsystems need per-frame mission updates. Rather than twelve `modded class MissionGameplay { override void OnUpdate }` blocks each re-resolving the player, input, focus and open menu, Core overrides `OnUpdate` once, resolves everything, and calls a virtual of its own:

```
void Expansion_OnUpdate(float timeslice, PlayerBase player, bool isAliveConscious,
                        Input input, bool inputIsFocused, UIScriptedMenu menu,
                        ExpansionScriptViewMenuBase viewMenu)
```
`DayZExpansion/Core/Scripts/5_Mission/DayZExpansion_Core/Mission/MissionGameplay.c:110`

Eleven subsystems then override `Expansion_OnUpdate` and chain through `super`: AI, BaseBuilding, Book, Chat, Groups, Hardline, NamalskAdventure, Navigation, P2PMarket, Quests and the root Scripts module. **Of the twelve files that reopen `MissionGameplay`, exactly one touches vanilla `OnUpdate`.** `DayZGame` uses the same trick, with the hook declared in the Core preload and the body added in Core Scripts.

`ExpansionSettings` is the same shape applied to initialisation: reopened in exactly 20 files, every one of them named `ExpansionSettings.c`, every one an `override void Init()` that calls `super.Init()` then registers its own settings class. The ordering problem that mod load order cannot express is solved with a conditional early call:

```
#ifdef EXPANSIONMODHARDLINE
    //! Need to load hardline before market so we have access to rarity for market items
    Init(ExpansionHardlineSettings, true);
#endif
    Init(ExpansionMarketSettings, true);
```
`DayZExpansion/Market/Scripts/3_Game/DayZExpansion_Market/Settings/ExpansionSettings.c:18-26`

180 `ScriptInvoker` references back this up as the general answer to "N modules want the same event without fighting over one override".

---

## Talking to code you do not own

### RPC by name hash

DayZ gives a mod one hook, `DayZGame.OnRPC(sender, target, rpc_type, ctx)`, with `rpc_type` as a bare int. Two mods that pick the same int collide silently. Expansion burns three ints and tunnels everything through them:

```
static const int EXPANSION_RPC_UNTARGETED = 1506850293;  //! "DayZ Expansion".Hash()
static const int EXPANSION_RPC_TARGETED   = 1506850294;  //! "DayZ Expansion".Hash() + 1
static const int EXPANSION_RPC_SJ         = 1506850295;  //! "DayZ Expansion".Hash() + 2
```
`DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/ExpansionScriptRPC.c:16-18`

Inside the tunnel the real ID is `("ClassName::functionName").Hash()` (`.../ExpansionRPCManager.c:205`), registered separately for the server and client directions, with collisions detected at registration and reported rather than mis-dispatched (`:229`, `:243`). Dispatch is reflective: `g_Game.GameScript.CallFunctionParams(instance, fn, null, params)` (`:558`, `:589`). The manager itself is found on an arbitrary entity by field name, `EnScript.GetClassVar(target, "m_Expansion_RPCManager", 0, manager)` (`:514`), so any class can host RPCs without sharing a base with anything.

Registration is keyed on the *moddable root* of the entity, not its concrete class, so a handler registered on `ItemBase` serves every subclass (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/ExpansionWorld.c:211-234`, walking `AdvancedCommunication`, `AnimalBase`, `CarScript`, `BoatScript`, `BuildingBase`, `DayZPlayerImplement`, `ItemBase`, `ZombieBase`).

The system refuses to coexist with the Community Framework module RPC system rather than half-working: `Error("Cannot use CF Module RPC system and Expansion RPC manager at the same time!")` at `DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/CF_ModuleGame.c:31` and `:42`.

Counts: 161 `Expansion_CreateRPC`, 86 `Expansion_RegisterServerRPC`, 67 `Expansion_RegisterClientRPC`, and **zero new members added to the vanilla `ERPCs` enum**. This is the single largest collision surface in DayZ modding and Expansion removed itself from it entirely.

On top of the vanilla serializer sits `DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/expansionbitstream.c`, 1,493 lines of sub-byte packing with fp16 and bfloat16 float encodings and alphabet-coded strings. Class names are common in DayZ RPCs, so `CLASSNAME_ALPHABET` at `:61` reduces each character to 6 bits.

### Reflection, for when there is no hook

`EnScript.GetClassVar` / `SetClassVar` are vanilla proto natives. Expansion uses them at 39 sites for three distinct purposes, and the comment on the third states the design rationale outright:

```
#ifdef HypeTrain
    //! Using SetClassVar so if HypeTrain ever renames the variable there will be no compile error (it'll still break though)
    EnScript.SetClassVar(this, "m_HypeTrain_PositionBuffer", 0, m_ExProjectionPosition);
#endif
```
`DayZExpansion/BaseBuilding/Scripts/4_World/DayZExpansion_BaseBuilding/Classes/BaseBuilding/Hologram.c:260`

No `requiredAddons`, no compile-time coupling, silently a no-op if that mod is absent. The other two uses are attaching state to a class you did not reopen (the RPC manager above) and generic settings serialization that walks a dotted path by splitting a string and re-entering `GetClassVar` per segment.

Beyond field access there is dynamic dispatch by name: `g_Script.CallFunction(g_Game.GetMission(), "GetNamEventManager", ...)` reaches into Namalsk Survival without linking to it, and the AI FSM builds states with `m_Module.CallFunctionParams(null, "Create_" + m_ClassName, retValue, params)` (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/FSM/ExpansionFSMType.c:80`).

### CfgMods as an extension registry

`CfgMods` accepts arbitrary keys and script can read them back. Three are in use here:

| Key | Declared | Read by |
|---|---|---|
| `expansionSkins[]` | `DayZExpansion/Scripts/config.cpp:152`, `Vehicles/Scripts/config.cpp:43`, `Weapons/Scripts/config.cpp:32` | `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/Systems/Skins/ExpansionSkinModule.c:45` |
| `roadNetworkDirectory` | third-party map mods | `DayZExpansion/AI/Scripts/4_World/DayZExpansion_AI/Classes/Roads/eAIRoadNetwork.c:47-50` |
| `CF_ModStorage=1` | `DayZExpansion/Core/Scripts/config.cpp:24` and others | Community Framework |

`roadNetworkDirectory` is the interesting one. Expansion's AI enumerates every loaded mod's `CfgMods` looking for that key, so any map author can publish road data to Expansion's pathfinding without either side knowing about the other.

The same idea appears on class configs. `DayZExpansion/VanillaFixes/Animals/config.cpp:22` sets a custom `useExpansionNavMeshCarver=1` on three animal classes and the script reads it back with `ConfigGetBool`. Config-tree reads run to 398 sites in 89 files across `ConfigGetChildrenCount`, `ConfigGetChildName`, `ConfigIsExisting`, `ConfigGetText`, `ConfigGetTextArray`, `ConfigGetVector`. This is the main way behaviour is declared per class without touching script. The limit is documented: **nested arrays in `config.cpp` are unreadable from script** (`DayZExpansion/AI/Scripts/4_World/DayZExpansion_AI/Entities/Weapons/Firearms/Weapon_Base.c:249`).

### Feature defines, both directions

Third-party gates in use, with file spread: `JM_COT` 67 sites in 55 files, `NAMALSK_SURVIVAL` 37 in 26, `CF_DebugUI` 33 in 29, plus `HypeTrain`, `VPPADMINTOOLS`, `COMPONENT_SYSTEM`, `NAMALSK_ISLAND`, `CARCOVER`, `DabsFramework`, `GAMELABS`, `TRADER`, `FALLUJAH_MAP`. The canonical form wraps the whole class, not the body:

```
#ifdef JM_COT
modded class JMPlayerModule
```
`DayZExpansion/AI/Scripts/5_Mission/DayZExpansion_AI/COT/JMPlayerModule.c:14`

More interesting is the class of define where the dependency publishes a flag saying it has *changed a signature*: `COT_BUGFIX_REF`, `CF_BUGFIX_REF`, `JM_COT_EXPTRANSITION_1`, `COT_UIACTIONS_SETWIDTH`, `CF_XML_READ_DOCUMENT_EX`, `CF_XML_GET_TAGS`. An override in Enforce must match its base signature exactly, `ref` included, so when a dependency drops a `ref`, every override in every dependent mod stops compiling and the only recovery is a define the dependency ships:

```
#ifdef COT_BUGFIX_REF
protected override bool SetModule(JMRenderableModuleBase mdl)
#else
protected override bool SetModule(ref JMRenderableModuleBase mdl)
#endif
```
`DayZExpansion/Groups/Scripts/5_Mission/DayZExpansion_Groups/COT/ExpansionCOTGroupsMenu.c:168-172`

---

## Data

### Persistence, two channels

**Entity-attached state** rides on the vanilla persistence blob through Community Framework's ModStorage. The namespace key is a `ModStructure` subclass acting as a typename, one per persisting subsystem, all wrapped in `#ifdef EXPANSION_MODSTORAGE`:

```
class DZ_Expansion_BaseBuilding : ModStructure
{
    override void LoadData()
    {
        super.LoadData();
        SetStorageVersion(52);
    }
};
```
`DayZExpansion/BaseBuilding/Scripts/3_Game/DayZExpansion_BaseBuilding/DZ_Expansion_BaseBuilding.c:13-23`

Use is a map lookup keyed by that bare typename with a null check that tolerates the addon being absent. 90 `CF_OnStoreSave` and 95 `CF_OnStoreLoad` sites exist. `ModStructure` is vanilla (`P:/scripts/3_game/client/mods/modstructure.c`); Expansion supplies only the version number and the key.

`PlayerBase` gets a full bypass of vanilla store save and load, with the reason stated at `0_DayZExpansion_Core_Preload/4_World/Entities/ManBase/PlayerBase.c:6-9`: vanilla "plays fairly loose with the way some of PlayerBase data is written to storage which makes reading it back safely a challenge".

**Everything else** is JSON or a `FileSerializer` binary under `$profile:ExpansionMod\`. The centrepiece is `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/ExpansionEntityStorage.c`, 1,330 lines at `VERSION = 13` (`:86`), which serialises an entity plus its whole attachment and cargo tree to a `.bin` and can rebuild it. It deliberately inverts vanilla's load order so children are restored before the parent's `OnStoreLoad` runs, and says why at `:80`: "This difference is intentional so that restored parent entity state is less likely to interfere with creation of child entities." Two `//! @note order of operations matters! DO NOT CHANGE!` markers sit at `:142` and `:504`, and a third at `.../Static/ExpansionItemSpawnHelper.c:639`.

### Versioning is hand-written, everywhere

48 files declare a `static const int VERSION`, and every persisted structure carries a manual migration ladder. `MarketSettings` is at 17 with an explicit chain for versions 2 through 15; `AISettings` at 20; `EntityStorage` at 13; quest configs run through five generations of base class (`ExpansionQuestConfigBase`, `V5Base`, `V5`, `V15Base`, `V19Base`) so old JSON still parses. The typical pattern is to load the file twice, once into an old-version class and once into the current one, and copy fields across. **This is the single largest recurring tax in the codebase and it is paid in every subsystem.**

---

## The interface layer

233 layouts, 55,360 lines of layout text, 3,413 widget entries, and about 33,700 lines of script in classes that own a layout. It is built on an MVC framework that **is not vanilla and is not Expansion's own**. `ScriptView`, `ViewController`, `ViewBinding` and `ScriptedViewBase` come from DabsFramework (`DabsFramework/Scripts/3_Game/DabsFramework/MVC/ScriptView.c:40`); grepping `P:/scripts` for `ScriptView` returns nothing. Expansion subclasses them in `DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/MVC/ScriptViews/Bases/`.

The whole layer is behind `#ifdef EXPANSIONUI` (38 sites in 28 files), which wraps entire class definitions. When the flag is off, the type degrades to a primitive so every signature that mentions it still compiles:

```
};
#else
//! Dummy for MissionGameplay::Expansion_OnUpdate
typedef bool ExpansionScriptViewMenuBase;
#endif
```
`DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/MVC/ScriptViews/Bases/ExpansionScriptViewMenuBase.c:34-37`

There are three parallel UI systems. 129 classes override `GetLayoutFile()` and 126 override `GetControllerType()` (DabsFramework MVC); 46 layouts are loaded by hand through `GetWorkspace().CreateWidgets`; 17 live Community Online Tools forms extend `JMFormBase`. A fourth style, `ScriptedWidgetEventHandler` subclasses, handles map overlays.

Three name-matched contracts bind a layout to a class, and the compiler checks none of them: the class returns a layout path, `ScriptView`'s constructor auto-wires widget-typed fields by `FindAnyWidget(propertyName)` plus `EnScript.SetClassVar`, and the layout's root widget names its controller in a `scriptclass` string. `Binding_Name` appears 472 times, `Relay_Command` 181, and `Two_Way_Binding` **6** times in 233 layouts.

The binding system covers about a quarter of the surface. There are 628 `FindAnyWidget` calls, and the reason is in what `ViewController` supports: string, bool, int, float, `Object`, `ObservableCollection` (94 references), and nothing else. **There is no colour binding, no visibility binding, no image binding.** So the theme is carried by hand: **1,129 `.SetColor(` calls and 984 `ARGB(` literals.** `ExpansionMarketMenuColorHandler` and `ExpansionATMMenuColorHandler` are plain classes, not views, that take a layout root and run 40-odd `FindAnyWidget` plus `SetColor` pairs each.

Nine layout files exist only because a number could not be set at runtime: `Chat/GUI/layouts/expansion_chat_entry_{verysmall,small,medium,large,verylarge}.layout`, five files of 148 lines each, byte-identical except a font size, and `Navigation/GUI/layouts/expansion_dynamic_marker_{16,24,32,40}.layout`. The engine does expose `TextWidget.SetTextExactSize(int)` (`P:/scripts/1_core/proto/enwidgets.c`), and all five chat layouts already set the `"exact text" 1` flag it requires. Expansion calls it twice in 337,448 lines.

Two structural facts worth stealing. First, vanilla `UIScriptedMenu` needs a globally unique integer with no registry, so Expansion reserved four IDs (`DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/ExpansionConstants.c:43-48`, 1000 to 1003) and moved everything else to class-name-keyed script views, which cannot collide. Second, `ExpansionScriptViewMenu::LockInputs` enumerates every active input from `GetUApi().GetActiveInputs()` and `ForceDisable`s everything outside a skip list, because a ScriptView is not a `UIScriptedMenu` and nothing disables player input for you.

What it costs. Every `ScriptView` joins the global update queue whether it needs to or not: DabsFramework's constructor inserts `Update` into `GetUpdateQueue(CALL_CATEGORY_SYSTEM)` when `UseUpdateLoop()` returns true, which is the default, and **`UseUpdateLoop` does not appear once in Expansion's 1,963 files.** Every market list row, quest entry and chat line is in the system update queue calling an empty method.

Localisation is 20 `stringtable.csv` files under `DayZExpansion/languagecore/<Subsystem>/`, one `config.cpp` each declaring only a `CfgPatches` entry, 1,908 keys, 13 languages. Splitting translations into 20 PBOs is what lets a server owner run the Market bundle without shipping the AI strings. Input binding is 11 `Inputs.xml` files declaring 76 distinct input names, wired through an `inputs=` key on `CfgMods`, with 10 of the 11 putting their entries in one `<sorting name="expansion">` group so the keybinds menu shows one section rather than eleven.

---

## Where the engine ran out

### AI: 47,873 lines to make a character walk

`eAIBase: PlayerBase` (`DayZExpansion/AI/Scripts/4_World/DayZExpansion_AI/Entities/AI/eAIBase.c`, 11,786 lines) means every AI is a full player character with inventory, damage zones, stamina and animation, driven server-side. Vanilla has no humanoid AI and no script path to "make a Man walk here and shoot that". Expansion never touches `AIAgent`; `eAIGroup` is a plain script class with its own ID counter, formations, waypoints and target list.

Three engine hooks carry it. `HumanCommandScript` lets script own the movement command slot. `HumanAnimInterface.BindCommand` binds commands and variables by string name, and the names Expansion binds do not exist in vanilla: `CMD_eAI_Turn`, `CMD_eAI_StopTurn`, and variables `eAI_AimX`, `eAI_Raised`, `eAI_Lean` (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/Commands/ExpansionHumanST.c:88-89` and following). So Expansion ships a replacement player animation graph as its own addon. **That is the most fragile dependency in the mod**, and the shipping runtime warning at `DayZExpansion/Core/Scripts/5_Mission/DayZExpansion_Core/Mission/MissionServer.c:177-187` exists for exactly this reason: it tells connecting players the server runs an unsupported game version, names a hardcoded `preferredVersion`, and adds a line about animations if the Animations preload is loaded. Third is `AIWorld` navmesh sampling, with a hand-written A\* over a separate road graph for the parts the navmesh does not model.

The most surprising thing in the whole corpus is how AI behaviour is authored. It is XML with editor coordinates, transpiled to Enforce Script at runtime, written to `$profile:ExpansionMod\AI\FSM\<name>.c`, and compiled in-process:

```
new_type.m_Module = ScriptModule.LoadScript(module, script_path, false);
```
`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/FSM/ExpansionFSMType.c:130`

The generator emits `Create_<class>` factory functions (`:274`) and instantiation is `CallFunctionParams(null, "Create_" + m_ClassName, ...)` (`:80`). The four shipped graphs are `Master.xml` (21 states, 41 transitions), `Vehicles.xml` (6, 11), `Reloading.xml` (4, 7), `Fighting.xml` (4, 5): **35 states, 64 transitions, 1,055 lines** under `DayZExpansion/AI/Scripts/FSM/`. Every `<state>` names a script class, every `<transition>` carries a `<guard>` block of raw Enforce, `from_state` accepts comma-separated lists, and every node carries `<editor_data><position/><size/></editor_data>`. Expansion built a visual FSM editor for this and the coordinates survived in the shipped data.

`ScriptModule.LoadScript` is the only way in the engine to change gameplay logic without rebuilding a PBO, and this is the only place in 337,448 lines that uses it.

### Vehicles: the retreat

Expansion originally shipped a complete script-side vehicle simulation with its own gearboxes, axles, aerofoils, buoyant points and a translated Bullet constraint solver. That code is still in the tree, under seven `Deprecated/` folders: **26 files, 12,561 lines**. What replaced it is `modded class CarScript` with an array of `ExpansionVehicleModule` and eight event buses (deferred init, settings changed, control, pre-simulate, simulate, animate, network send, network receive). A helicopter is now a `CarScript` with lift modules attached.

The composition trick is worth copying. `CarScript` and `BoatScript` share no useful base and Enforce has no mixins, so the shared state lives in a generic component instantiated on each:

```
ref ExpansionVehicle m_ExpansionVehicle = new ExpansionVehicleT<CarScript>(this);
```
`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Entities/Vehicles/CarScript.c:33` and `BoatScript.c:18`, with `class ExpansionVehicleT<Class T>: ExpansionVehicle` at `.../Classes/ExpansionVehicle.c:937`

Locking, ownership, keychains, safezone state, lifetime and last-driver tracking all live in that component. The corpus has 21 generic class definitions and 10 container-alias classes (`class ExpansionCEEvents: map<string, ref ExpansionCEEvent>`) plus 26 `typedef` aliases naming concrete instantiations.

**Do not mine the `Deprecated/` folder for patterns.** Its replacements total a few hundred lines of stubs, and 12,561 lines of abandoned approach will teach a palette things Expansion itself gave up on.

### Garage: what the storage layer buys you

17 files, 3,472 lines, and the clearest illustration of the persistence design. Storing a vehicle serialises it to `$profile:ExpansionMod\EntityStorage\<hexid>.bin` and replaces it in the world with an `ExpansionEntityStoragePlaceholder`: an inert item that carries the stored entity's 128-bit global ID and its type name in `ExpansionNetsyncData` so clients can show the right display name, inherits the stored entity's `GetLifetimeMax` so the central economy does not clean it early, refuses all inventory interaction, deletes its `.bin` in `EEDelete`, and deletes itself on load if the `.bin` is gone. That is how you keep a vehicle out of the world without lying to the central economy about what exists.

The netsync trick behind it is its own lesson. `RegisterNetSyncVariable*` covers bool, int and float; there is no string. `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/ExpansionNetsyncData.c` replaces it with a client-pull RPC, and two of its details are hard-won:

```
g_Game.GetCallQueue(CALL_CATEGORY_SYSTEM).CallLater(LateClientInit, 250);  //! Has to be delayed, else won't work!
```
`:43`

```
//! if you take an item to hand, vanilla will create a temporary copy of the item on client only.
//! We can detect such an item by checking its network ID. If it's client only, net ID will be zero.
```
`:47-51`

There are 185 real `RegisterNetSyncVariable` calls, and the sync name can be a dotted path into a member object (`vehicle.RegisterNetSyncVariableInt("m_ExpansionVehicle.m_MasterKeyPersistentIDA")`), which is the one genuinely generous piece of that API.

### The compile-time and runtime split

There is no physical client and server separation. The same PBOs go to both sides.

| Guard | Sites |
|---|---:|
| `#ifdef SERVER` | 125 |
| `#ifndef SERVER` | 144 |
| `#ifdef CLIENT` | **0** |
| `#ifdef DIAG_DEVELOPER` | 594 |
| `#ifdef EXTRACE` | 1,194 |
| `#ifdef EXPANSIONTRACE` | 808 |

There is no `CLIENT` define in DayZ. Client-only code is `#ifndef SERVER`.

Runtime checks dominate: `IsServer()` 412 sites, `IsMultiplayer()` 180, `IsMissionHost()` 162, `IsDedicatedServer()` 158, `IsMissionClient()` 117, `IsClient()` 78. The division of labour is legible. `#ifdef SERVER` is used where the code must not *exist* on the other side, most clearly for symmetric declarations that mean opposite things per side (`ExpansionRPCManager.c:414-423`) and for the rate-limited server update loop at `0_DayZExpansion_Core_Preload/3_game/dayzgame.c:7-28`. `IsServer()` guards writes to authoritative state in code that compiles on both sides. `IsMissionHost()` is the lifecycle one, true on a dedicated server and a singleplayer host, and all eight `modded class DayZExpansion` blocks use it and nothing else.

Tracing is its own layer and its own lesson about what Enforce can do without macros. `EXTrace.Start(...)` assigned to an `auto` local relies on **deterministic destruction of a ref-counted local at scope exit**; the destructor at `DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/ExpansionTracing.c:210` prints the exit line and the elapsed time. 1,632 `EXTrace.Start`-family sites plus 892 `CF_Trace_*` sites means this scope-guard shape appears more often than `IsServer()`. The file also shows that **static field initializers run a static method at class-load time**: 48 category flags all initialised from `IsEnabled()` at `:34` onward, which reads `#ifdef DIAG`, `BUILD_EXPERIMENTAL` and `GetCLIParam("extrace", ...)` once and caches. The file's own warning at `:28` says not to use it unguarded in `OnUpdate`.

Other logging: 510 `EXPrint`, 411 `EXLogPrint`, 47 `ErrorOnce`/`WarnOnce`.

---

## What will waste your day

Engine and compiler behaviours proven by these files. Each line has a path behind it.

1. **`bool x = a && b;` can produce the wrong value.** Write `bool x; if (a && b) x = true;` instead. T173348, cited 32 times in 24 files, and the avoidance idiom appears at least 101 times in 57 files. Canonical shape at `DayZExpansion/BaseBuilding/Scripts/4_World/DayZExpansion_BaseBuilding/Entities/Basebuilding/Walls/ExpansionWall.c:171-177`.
2. **Never nest a call inside another call's arguments or a return.** `f(g())` can pass null (`DayZExpansion/Market/Scripts/5_Mission/DayZExpansion_Market/Market/ExpansionMarketMenuItem.c:466`), `return super.f()` can return null (`DayZExpansion/Scripts/5_Mission/DayZExpansion/GUI/ServerBrowserMenu/ServerBrowserMenuNew.c:58`), and `vector.Direction(a, b.GetPosition()).Normalized()` returns a zero vector (`DayZExpansion/AI/Scripts/4_World/DayZExpansion_AI/Entities/AI/eAIBase.c:8385`). Assign to a named local first. One root cause: the compiler mishandles temporaries in nested expressions.
3. **`Math.Floor` is wrong for large floats.** `float.MAX` comes back halved. Use `-Math.Ceil(-x)` (`DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/ExpansionMath.c:558-559`).
4. **Comparing two ints of opposite sign is unreliable.** Use a sign-branching compare (T167065, `ExpansionMath.c:504-510`).
5. **`string.Replace` truncates long text** (T177558, `DayZExpansion/Core/Scripts/1_Core/DayZExpansion_Core/ExpansionString.c:225-226`). **`Math.Log2` is integer-only despite a float signature** (`ExpansionMath.c:571`). **`Math.RandomFloatInclusive` excludes the endpoint** (`ExpansionMath.c:530`).
6. **`Print()` truncates at 1026 characters on the server and 240 on the client.** Different limits, undocumented (`DayZExpansion/Core/Scripts/3_Game/DayZExpansion_Core/ExpansionStatic.c:36-38`).
7. **`super` is optional in vanilla and often absent.** 55 comments in 30 files record it. Vanilla `Torch::OnWorkStart` is `m_WasLit = true; LockRags(true); UpdateMaterial();` with no super call (`P:/scripts/4_world/entities/itembase/torch.c:523-528`). Because there is no engine event for "an item started working", Expansion patches 26 leaf item classes by hand under `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Entities/ItemBase/`, each an identical 18-line file. Check the vanilla source of any method you override before assuming your hook fires.
8. **Some classes refuse `modded class`.** `ItemOptics` and `HumanInventory` are the documented cases. Patch the nearest moddable subclass and accept the gap (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Entities/ItemBase/poweredoptic_base.c:3`, `.../Entities/ManBase/PlayerBase.c:1149`).
9. **`EEKilled` passes the dead entity as its own killer since 1.20.** Cache the source in `EEOnDamageCalculated` (`DayZExpansion/VanillaFixes/Scripts/4_World/DayZExpansion_VanillaFixes/Entities/Creatures/Animals/AnimalBase.c:28-33`). The same file documents the real per-hit event order, which is in no doc: `EEOnDamageCalculated`, then `EEHitBy` with `IsAlive()` still true, and `EEKilled` fires *inside* `EEHitBy`. Because `EEKilled(Object killer)` has a fixed signature, the correction cannot be centralised and every consumer repeats it (Hardline and Quests both carry their own copy).
10. **`ProcessDirectDamage` segfaults the server when the source is a static or baked entity.** Gate on the entity kind (T192088, `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/Systems/ExpansionDamageSystem.c:344`, `DayZExpansion/AI/Scripts/4_World/DayZExpansion_AI/Classes/eAIDamageHandler.c:482`).
11. **`TakeToDst` returns true and silently does nothing** when called more than once per tick, because `FindFreeLocationFor` keeps returning the same location. Create the entity at the destination instead (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Static/ExpansionItemSpawnHelper.c:355-357`).
12. **Item lifetime is always zero in `EEOnCECreate`.** Wait for the first `OnCEUpdate` (T194225, `DayZExpansion/VanillaFixes/Scripts/4_World/DayZExpansion_VanillaFixes/Entities/itembase.c:21-26`).
13. **Passing null as an array parameter can null the caller's array.** Copy first (T173458).
14. **Nested arrays in `config.cpp` are unreadable from script** (`DayZExpansion/AI/Scripts/4_World/DayZExpansion_AI/Entities/Weapons/Firearms/Weapon_Base.c:249`).
15. **Vanilla `ErrorEx` fires per event, not once.** One modded object missing a `Hit_` material floods the log at the weapon's fire rate (`P:/scripts/3_game/impacteffects.c:171,182,190`). Expansion pre-resolves the lookup, emits one `ErrorOnce`, and rewrites the surface string so vanilla's lookup succeeds quietly (`DayZExpansion/VanillaFixes/Scripts/3_Game/DayZExpansion_VanillaFixes/DayZGame.c:131-169`).
16. **The serialiser cannot seek backwards.** `ctx.Write("");  //! Have to write empty entry because we already have written inventory count and no way to retroactively overwrite it` (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/ExpansionEntityStorage.c:226`).
17. **Layouts have no inheritance and no partial override.** Changing one attribute means copying the whole file and owning it forever. `DayZExpansion/VanillaFixes/GUI/layouts/new_ui/options/keybindings_selectors/keybinding_option.layout` is 166 lines against vanilla's 169, and the entire substantive difference is two deleted `ignorepointer 1` lines, three `clipchildren` flips and two colour values.
18. **Reaching into a vanilla layout means a string.** `m_HudPanelWidget.FindAnyWidget("BadgeNotifierDivider")` is the only way to recolour a vanilla widget, and it returns null the day Bohemia renames it.
19. **When in doubt, defer one frame.** 309 `GetCallQueue` sites and 220 `CallLater` calls. Named traps: setting plant health in the contact frame segfaults the server under 1.28 experimental; `ClearFlags` in `EEItemLocationChanged` must be deferred or other clients stop seeing items in hands, and reaching `ClearFlags` through `ScriptCallQueue::Call` crashes them instead (`DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Classes/ExpansionOwnedContainer.c:386-388`). Some delay values are load-bearing magic numbers (the 250 ms at `ExpansionNetsyncData.c:43`).
20. **Guard every destructor with `if (!g_Game) return;`.** During shutdown the global is gone. There are 220 destructors in the corpus and 44 carry that guard.
21. **Not removing a call-queue entry in a destructor is a shipped vanilla bug.** `//! Fix vanilla not removing PosUpdate from call queue if InfectedSoundEventBase::Stop was never called` (`DayZExpansion/VanillaFixes/Scripts/4_World/DayZExpansion_VanillaFixes/Classes/soundevents/infectedsoundevents/infectedsoundeventbase.c:1`).
22. **Engine version gates change the shape of your code, not just a branch.** 63 `#ifdef DAYZ_1_XX` sites across 30 files, from `DAYZ_1_18` to `DAYZ_1_28`, and 22 of the 30 files are in Vehicles. `DAYZ_1_XX` is supplied by the engine, not by the mod (`P:/scripts/1_core/defines.c:1-14`, "All defines in this file are added from C++ side"). Real signature changes hide under them: `m_HeadlightsOn` became `LightIsOn()` at 1.29, `set.Remove` gained `RemoveItem` at 1.21.
23. **Your fixes rot, and nothing tells you.** `DayZExpansion/VanillaFixes/Scripts/5_Mission/DayZExpansion_VanillaFixes/GUI/IngameHud.c:20-25` overrides `SetLeftStatsVisibility` and calls `m_LeftHudPanelWidget.Show(visible)`. In current vanilla, both names sit inside the block headed `// everything below is DEPRECATED` at `P:/scripts/5_mission/gui/ingamehud.c:1270`: `m_LeftHudPanelWidget` is declared at `:1273` and never assigned, and `SetLeftStatsVisibility` is declared with no body at `:1306`. The fix targets a field the engine no longer populates. **Re-diff your fix folder against vanilla at every DayZ release.** The comment above it, dated two days apart, is worth reading in full for the tone of the whole exercise.
24. **A missing `requiredAddons` edge can work by accident.** `DayZExpansion/MapAssets/Scripts/config.cpp:8` declares `requiredAddons[]={}` yet its `4_World` code calls `ExpansionPointLight`, defined in `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/Lights/ExpansionPointLight.c:16`. Symbol resolution is module-wide so it compiles regardless of order. What actually saves it is that both call sites are wrapped in `#ifdef EXPANSIONMODCORE`, which is the Core *preload*'s define, not the Core Scripts PBO's. Those two sites are the only consumers of that flag in the corpus.
25. **A duplicate `CfgMods` class name merges silently.** `0_DayZExpansion_Teleporter_Preload/config.cpp:13` declares `class DZ_Expansion_PersonalStorage_Preload`, the same name already used at `0_DayZExpansion_PersonalStorage_Preload/config.cpp:13`. The `CfgPatches` entry is correct so the PBO loads, but the two `CfgMods` classes merge under one name, and `ConfigIsExisting("CfgMods DZ_Expansion_Teleporter_Preload")` returns false. It is still shipping.
26. **A module flag can be referenced and never defined, and nothing warns.** `EXPANSIONMODGARAGE` has 7 `#ifdef` sites in 6 files and no `#define` anywhere, so `ExpansionParkingMeter` and `ExpansionParkingMeterKit` never exist as script classes. `EXPANSIONMODP2PMARKET` is in the same state with 2 dead files.
27. **A dead widget field looks like working code.** `DayZExpansion/Teleporter/Scripts/5_Mission/DayZ_Expansion_Teleporter/GUI/ExpansionTeleporterMenu.c:31-33` declares `Complete`, `CompleteLable` and `CompleteBackground`. The layout has `Accept`, `AcceptLable`, `AcceptBackground` and nothing named `Complete`. Lines `:311-314` and `:353-356` compare `w == Complete` against a permanently-null field, so a branch that reads like working hover code has been unreachable since the rename.
28. **The layer where you put a method is constrained by things other than taste.** `//! @note Can't be on PlayerBase, leads to compile error due load order :-(` forces a method to be a static on another class. `DayZExpansion/Core/Scripts/4_World/DayZExpansion_Core/ExpansionWorld.c:236` records a function moved off `Liquid` because an unrelated *private* method on `Liquid` caused a compile error.

The most human artefact in the corpus, at `DayZExpansion/BaseBuilding/Scripts/4_World/DayZExpansion_BaseBuilding/Classes/BaseBuilding/ExpansionBaseBuilding.c:258-259`, is a comment saying that doing something fixed a null pointer, that it is 3AM, and good luck. It now sits above an unrelated method. That is the maintenance model in one screenshot.

---

## What this means for the node tool

The rest of this file is a work list for SUDO DayZ Node Mod, ranked by the count behind each item. Some of it is built and some is not; the paths are into this repository, and where one names a line number it was true when the item was written rather than being kept current.

**1. A `fmt.braces` option on branch nodes. 10,654 sites.**
45.0 percent of Expansion's control-flow headers that stand on their own line are written without braces (10,654 of 23,688). `src/codegen.cpp:765-781` emits `{` and `}` for `bi.branch` unconditionally and there is no opt-out. `nodefmt` already carries `fmt.base`, `fmt.unit`, `trivia.before`, `trivia.trailing`, `trivia.end` and `trivia.endElse` on the node; a `fmt.braces` key beside them is the cheapest large win available, and by the tool-fit harness it is worth roughly twelve points on the method import rate on its own.

**2. `#ifdef` region tracking, and no macro expander. 5,701 guard sites, 859 files, 169 distinct flags, 78 defines, zero macros.**
This is two findings in one. All 78 `#define` sites in 337,448 lines are bare flags: zero take arguments, zero have a replacement body, and the same holds for all of `P:/scripts`. **An entire subsystem can be deleted from the importer.** What is needed instead is `#ifdef`/`#ifndef`/`#else`/`#endif` region handling, because guards wrap whole classes (80 class declarations are gated this way), whole methods, and single statements. A parser that treats preprocessor lines as whitespace produces class graphs that do not match the compiled binary, and a `modded class` under `#ifdef JM_COT` that loses its guard breaks 55 files on export.

**3. Whole-mod indexing plus a base-chain walk in the resolver. 35 percent of all lowering decline weight.**
The single largest cause of a statement failing to become a node is "the call is not in the catalogue", and the top two unresolved names are declared *inside this corpus*, on a base class, called from sibling files. Loading the mod into `Project` is half the fix. The other half is in `src/enforce/lower.cpp:2608`: `functionIn` iterates only `s.graph.functions` for one exact-name-matched script, and never walks `graph.baseClass` up through the project. A subclass calling an inherited helper misses even with every file loaded.

**4. `modded class` as its own node kind, merged by target, with `super` defaulting on. 731 sites, 666 files, 448 targets.**
`PlayerBase` is reopened from 22 separate files, `ExpansionSettings` from 20, `ActionConstructor` 16, `ItemBase` 15, `MissionGameplay` 12, `IngameHud` 9. An importer that treats each as a distinct type produces 22 unrelated graphs for one runtime class; merge by target and keep the source addon as node metadata. And make "call parent" the default: inside `modded class` blocks Expansion calls `super` in about 80 percent of overrides, against roughly 29 percent elsewhere. A graph that makes it a deliberate extra step will emit mod-breaking code by omission. Ship the known-vanilla-omits-super list as data (26 item classes for `OnWorkStart`/`OnWorkStop` alone) and flag it per class.

**5. Emit `g_Game`, never `GetGame()`, and never a nested call in an argument or return. 2,971 lines against 0.**
Expansion has completed the migration and vanilla has largely followed. Separately, three independent compiler failure modes (see What will waste your day, items 1 and 2) mean generated code must spill every intermediate into a named local and must never write `bool x = <compound expression>;`. For a node graph this is free, since every wire already implies a value that can be named. The corollary is that the **importer** has to recognise the four-line `bool x; if (...) x = true;` shape as one boolean-assignment node, or every graph imported from a real mod is a hundred nodes noisier than it should be.

**6. A scope-guard node. 2,524 sites.**
1,632 `EXTrace.Start`-family plus 892 `CF_Trace_*`, all of the form `auto x = Type(...)` relying on destructor-at-scope-exit. That is the language's replacement for a statement macro and it appears more often than `IsServer()`. `auto` itself needs to be in the type model (3,448 sites), as do generics (21 `class X<Class T>` definitions, 10 `class X: array<Y>` aliases, 26 `typedef` concretisations).

**7. Two distinct nodes for server and client, not one. 269 compile-time sites in 120 files against 412 runtime sites.**
`#ifdef SERVER` and `#ifndef SERVER` remove code at compile time; `IsServer()` branches at runtime; `IsMissionHost()` (162) is the lifecycle one and is what all eight `modded class DayZExpansion` blocks use. They are not interchangeable, and a tool that renders them all as one "is server?" node will emit code that crashes the diag server, which Expansion documents at `0_DayZExpansion_Core_Preload/Common/DayZExpansion_Core_Defines.c:87`. There is no `CLIENT` define.

**8. Deferral as a first-class node. 309 `GetCallQueue`, 220 `CallLater`.**
The queue category is a required argument (`CALL_CATEGORY_SYSTEM`, `_GAMEPLAY`, `_GUI`), the delay value is sometimes load-bearing, and repeat is a flag. Include the destructor-side `Remove` / `RemoveByName` counterpart, because forgetting it is a shipped vanilla bug.

**9. RPC nodes carry a string name, not an integer. 161 create, 86 server register, 67 client register, 0 additions to `ERPCs`.**
Register and send are two node types. The handler signature is fixed. A "declare RPC" node that emits the registration in `OnInit` and stubs the handler with the right signature covers the whole pattern; one Market method registers 23 in a row. Mark the direct vanilla `ScriptRPC` path (11 sites) as legacy.

**10. A `ScriptView` node that generates its widget pins from the layout. 129 layout overrides, 126 controller overrides, 628 `FindAnyWidget` calls.**
The layout path, the controller typename and the auto-wired widget fields are one thing, not three. If pins are read out of the `.layout`, the dead-field failure class disappears. The binding surface is small enough to model completely: 472 `Binding_Name`, 181 `Relay_Command`, 94 `ObservableCollection`, and six real property types. Two-way binding appears 6 times in 233 layouts, so make it a checkbox, not a concept.

**11. Colour and hover need first-class treatment because the framework has none. 1,129 `SetColor` calls, 984 `ARGB` literals.**
`ViewController` has no colour type and no visibility type, which is why the theme is carried by hand and why two `ColorHandler` classes exist that are not views at all. A palette resource plus a colour pin retires them. Hover is 96 `OnMouseEnter` and 95 `OnMouseLeave` overrides, nearly all a `switch (w)` swapping two `ARGB` values, and collapses into one node.

**12. Ship a CF module template and a preload addon template. 31 modules, 25 preloads.**
31 classes extend `CF_ModuleWorld` or `CF_ModuleGame`, all registered by a `[CF_RegisterModule(...)]` attribute, all with the same skeleton. That is the single most repeated architectural unit in third-party DayZ code. The preload is scaffolding, not a node: `config.cpp` with empty `requiredAddons` and all five `class *ScriptModule` blocks each listing `Common` first, plus `Common/<Name>_Defines.c`. Getting the `0_` prefix or the `Common` repetition wrong is silent. Generate the `CfgPatches`-only stub addon too (`DayZExpansion/Core/Stub/config.cpp`, 14 lines, three dependents in this tree) as the way to publish a stable dependency name.

**13. Two nodes for exporting a compile-time flag, not one. 78 `#define` sites against 6 populated `defines[]` arrays.**
`defines[]` in `CfgMods` is global and flat-string only; `#define` in a preload `Common` file is order-dependent and supports nesting on `SERVER`, `DEVELOPER`, `WORKBENCH`, `DabsFramework`, `CF_DebugUI`. They are not substitutes and the difference is invisible until something fails to compile. Any node that emits a `#define` must know which addon and which `Common/` file it belongs in.

**14. Config-tree read nodes. 398 sites in 89 files.**
`ConfigGetChildrenCount`, `ConfigGetChildName`, `ConfigIsExisting`, `ConfigGetText`, `ConfigGetTextArray`, `ConfigGetBool`, `ConfigGetVector`. This is how a mod parameterises behaviour per class without a script subclass, and how `CfgMods` becomes an extension registry. Encode the limit too: nested arrays are unreadable.

**15. Persistence needs both channels. 90 `CF_OnStoreSave` plus 95 `CF_OnStoreLoad`, against JSON and `FileSerializer` file IO.**
A single "save" node covering one of them is wrong half the time. The ModStorage side needs the `ModStructure` typename key and the `SetStorageVersion` call; the file side needs the `$profile:` / `$mission:` path and a version field, because 48 classes carry a hand-written `VERSION` and a migration ladder.

**16. A graph validator, because none of this fails at compile time.**
One pass over this corpus found: a duplicate `CfgMods` class name, two never-defined but nine-times-referenced module flags, an absent `requiredAddons` edge held together by a define, 497 declared-but-absent script folders, 4 case-mismatched folder declarations, 40 orphaned layouts, 4 dangling `#STR_` keys, 3 uses of a misspelled widget style, and at least one dead widget field driving unreachable branches. All string mismatches, all shipping. Name checking at graph time is the tool's clearest value proposition.

**17. Import the AI FSM XML. 35 states, 64 transitions, 1,055 lines, editor coordinates included.**
`DayZExpansion/AI/Scripts/FSM/*.xml` is a node graph with `<editor_data><position x= y=/><size width= height=/></editor_data>` on every node and a `<guard>` block of raw Enforce on every edge. It is a concrete import target with real shipped data behind it, and it validates the round trip, because Expansion already transpiles it to Enforce and compiles it with `ScriptModule.LoadScript`.

**18. Small palette gaps, named by the corpus.**
`Class.CastTo` is 1,154 occurrences and is a two-output node (bool plus typed value), not an expression; getting it wrong makes every imported graph unreadable. `switch` must accept a `typename` selector with bare class names as case labels. Overloading needs real support: a palette keyed on method name alone collapses `Write` and `Read` (22 signatures each on one class). Class headers must accept generics: 21 `<Class T>` definitions and 10 `: array<...>` aliases are otherwise dropped silently. Destructors need a node path at all (220 in the corpus, currently gated off at `src/enforce/import.cpp:1089`). Break and Continue nodes do not exist. Reflection escape hatches (`EnScript.GetClassVar`/`SetClassVar`, 39 sites) are rare per site but load-bearing, and a graph importer that chokes on them fails on the most interesting files rather than the boring ones.

**19. Two things not to build.**
Do not build `2_GameLib` nodes: declared 68 times, populated zero times, in the largest DayZ mod there is. Do not mine `DayZExpansion/Vehicles/.../Deprecated/`: 26 files and 12,561 lines that Expansion itself abandoned, and their replacements are a few hundred lines of stubs.

**20. One performance note, measured against this corpus.**
Importing all 1,963 files takes about seven minutes, but `eAIBase.c` (352 KB, one class, 348 methods) takes 13 seconds on its own, because `src/enforce/import.cpp` regenerates the whole class once per method per candidate shape (the `generateEnforce(scratchWith(shell, asText), ...)` call around `:980`). Opening the biggest file in the biggest mod is a 13-second stall. Also: publish statement coverage, not method coverage. Roughly 39 percent of methods but only 5.5 percent of statements become nodes, because accepted methods average about 2 statements and refused ones about 13. And note that this corpus does not exercise line endings at all: **every file is LF, none is CRLF, none mixes them**, so the CRLF guarantees still need the vanilla corpus to test against.
