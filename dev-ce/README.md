# Dev CE (experimental)

A fork of [Wild-W's Civilization VI Community Extension](https://github.com/Wild-W/CivilizationVI_CommunityExtension) that adds **251 engine functions to 29 existing Lua objects**
(`City`, `Unit`, `Player`, `Game`, `Map`, ...). The functions are described in a generated table (`DevNativeTable.cpp`, made by `dev-ce/tools/gen_bridge.py`) and called through one generic,
tested dispatcher (`DevBridge.cpp`), instead of one hand-written wrapper each.
Licence: **AGPL-3.0**, same as upstream. Not endorsed by Wild-W, Firaxis or 2K.

**Looking up a function?** The [GameCore reference](https://cru121.github.io/civ6-gamecore-reference/) has a [Dev CE section](https://cru121.github.io/civ6-gamecore-reference/devce/) with one page per Lua object:
every exposed method with its engine signature, argument and return meaning, test status and an (AI-written, flagged inferred) summary of what the engine function does. The same reference documents the vanilla Lua API,
the engine classes behind it and the address mapping this fork uses ([`data/`](https://github.com/cru121/civ6-gamecore-reference/tree/main/data)).

## Read this first
* **Experimental. Single player only.** Many functions change game state directly and can desync multiplayer. There is no multiplayer guard.
* **Steam build 15038592 only** (GameCore build stamp is checked at startup; on any other build the bridge disables itself and says so in `DevBridge.log`).
* **Do not save a game you care about while testing.** Some functions have lasting effects.
* **Known unsafe functions:** `Unit.ChangeSightRange` (hung the game once on a late-game save; also reveals tiles and can meet city states),
  `PlayerTrade.ChangeDomesticTradeDisabledCount`, `ChangeInternationalMajorsTradeDisabledCount`, `ChangeInternationalMinorsTradeDisabledCount` (+1 disables trade routes, -1 does not bring them back).
* **Arguments are not range-checked.** An out-of-range id or index can crash the engine. A guard turns hardware faults (access violations) into a Lua error and a `FAULT` line in `DevBridge.log`,
  but it cannot catch silent memory corruption. 216 of the 251 functions return nothing, so you will not see whether they worked unless you read the state some other way.
* Only **one** GameCore-replacing mod may be enabled (Dev CE, Dev CE test build, the Community Extension, ... never two).

## Try it: Dev CE Playground
`dev-ce/mod/DevCE_Playground` is a small demo mod with an always-visible button panel (right edge of the screen). Enable it together with one of the Dev CE mods below. Buttons: Golden / Dark Age on and off for yourself (`GameEras:SetGoldenAge` / `SetDarkAge`),
Era +1 / -1 (`GameEras:SetCurrentEra`, also reaches eras added by other mods), spawn James of St. George (vanilla), action charges of the selected unit (vanilla `Unit:ChangeActionCharges` next to Dev CE's `UnitGreatPerson:ChangeActionCharges`,
which changes the great person's *Actions* count that vanilla Lua cannot reach), capital food +/-2 and trade route capacity +/-1. Each press reports before/after values in the panel and in `Lua.log`.
What the demo showed: the era and age functions are raw setters (no era-change popup, no dedication choice, no era score, and Golden and Dark can both be on at once); the charges, food and trade functions do exactly what they say and reverse cleanly.
It changes your game directly: single player, disposable games only.

## The two mods
| mod folder | what it is |
|---|---|
| `dev-ce/mod/DevCE` | the bridge, nothing runs by itself: call the new methods from your own scripts |
| `dev-ce/mod/DevCE_Test` | the same DLL plus self-test scripts that run at the start of your first turns and write results to `Lua.log` (lines with `DevCE`) and `DevBridge.log` |

Both need the built DLL in `Binaries/Win64/` (build instructions below; the repo does not contain binaries).

## Troubleshooting and bug reports
Everything useful goes to **`DevBridge.log`, in `Binaries/Win64` next to the DLL** (inside the mod folder under `Documents/My Games/Sid Meier's Civilization VI/Mods/`). It records the DLL version, the GameCore build stamp,
how many functions are ready or disabled, one line per interface registered on every game load, processor diagnostics (`[GameProcessor]`), and a `FAULT` line (function, `this`, argument values) whenever the crash guard catches something.
For the test mod also include the `DevCE` lines from `Logs/Lua.log`.

If the game **hangs or crashes** with no useful line: create an empty file named `DevBridge.trace` next to the DLL (or call `DevCE_Trace(true)` from Lua) and start again. Every call into the bridge is then written to
`DevBridge.log` *before* it runs, so the last `[trace]` line names the call that never came back. Delete the file again afterwards, it makes the log large.

A bug report needs: what you did, `DevBridge.log`, the `DevCE` lines of `Lua.log`, and whether the game was a new game or a loaded save.

## Calling the functions
They are normal methods on the objects you already have, named like the engine functions (`city:ChangeYieldChange(...)`, `Game.IncrementGameStateLock()`). The full list with C++ names, signatures, argument and
return kinds is `dev-ce/data/exposed.json`; `dev-ce/data/function_status.json` says how far each one has been tested. Function summaries written by an AI from decompiled code are marked inferred in the docs.

## What was tested (details and raw logs: `dev-ce/RESULTS.md`, `dev-ce/data/`)
* Startup address check 251/251; methods present on the Lua objects 226 (the rest are `Game`/`Map` static tables the test script cannot enumerate, or objects with no live instance in the test game).
* Oracle: 23 read-only engine getters give exactly the same value as the vanilla Lua getter, in a new game and a 400-turn game (covers plain and virtual `this`, fixed-point returns).
* Argument plumbing (Frida stubs): 212 pass, 27 deliberately excluded (engine-hot functions), 12 not runnable (no District/Deal/Territory/AreaPortal instance found by the test script).
* Real execution: 112 `Change*` functions called with +1 then -1 on a late-game save; no crash, 109 restored, 27 with a visible effect through vanilla getters, 3 not restorable (above).
* Hostile arguments: 2,263 calls with wrong object kinds, missing or garbage arguments: all raised a clean Lua error; 5 extreme-value calls on one getter faulted and were caught by the guard.
* Processor fix (`RegisterProcessor`, CE issue #5): handlers run, return values come back, 11 congress choosers called in a late-game World Congress.
* Save, quit to menu, load, new game: hooks register identically every time.
* **Not tested:** multiplayer, loading a save made after a mutator ran in plain CE or vanilla, other game builds, other people's machines, handlers that claim congress decisions.

## Licence, changes and provenance
* **Licence.** This repository is a fork of the Community Extension and therefore **AGPL-3.0** (`LICENSE.txt`). Files that are new in Dev CE carry an `SPDX-License-Identifier: AGPL-3.0-only` header;
  files that come from upstream keep upstream's text and Wild-W's copyright. If you give anyone a built DLL, they must be able to get the corresponding source (link this repository).
* **Changes relative to upstream** (as of 2026-10-05, see the git history for the details): new files `DevBridge.cpp/.h`, `DevNativeTable.cpp` (generated), `GameProcessor.cpp/.h`;
  modified `Main.cpp` (initialises the bridge and the processor, registers the test globals), `EventSystems.cpp/.h` (rewritten: `RegisterProcessor` goes through the game's own dispatcher, fixes CE issue #5),
  `AI.cpp` (`Handled` key for congress choosers), `UnitManager.cpp/.h` (`ChangeBuildCharges`), the project file (new sources), `.gitignore` (`asmjit/x86` is no longer ignored); everything under `dev-ce/` is new.
* **Where the function names and addresses come from.** The names (`GameCore::Player::Culture::GetCultureYield`, ...) were recovered from debug symbols that shipped with an older build of the game, following the method in the
  Community Extension contributor's guide, and mapped to the current Steam build 15038592 by byte-for-byte matching and call-graph analysis; struct layouts were cross-checked against the Linux port's debug information.
  The addresses are therefore only valid for that build. The mapping is published in [civ6-gamecore-reference](https://github.com/cru121/civ6-gamecore-reference) (`data/`).
* **What is not here.** No game binaries, assets or scripts, no decompiled code, and no Firaxis source. The table in `DevNativeTable.cpp` contains function names, addresses, short prologue bytes (to detect a wrong build) and argument kinds.
  Descriptions of what a function does were written by an AI assistant from decompiled code and are marked inferred; they are guesses until a test shows an effect.
* Unofficial fan project. **Not affiliated with or endorsed by Firaxis Games, 2K or Take-Two; not endorsed by the Community Extension's author.** Use it for modding and understanding the game; do not use it to cheat in multiplayer or to circumvent protection.

## Build
Visual Studio 2022 Build Tools, x64 Release, project `CivilizationVI_CommunityExtension.vcxproj`, with capstone and MinHook on the linker path (`CL=/DCAPSTONE_STATIC`).
Output name `GameCore_XP2_CE_FinalRelease.dll`. `asmjit/x86` is un-ignored in `.gitignore` (the upstream ignore rule `x86/` otherwise drops it from a clone).

## Tools (`dev-ce/tools`)
`gen_bridge.py` (table generator), `gen_selftest.py` (test mod generator, levels 1 to 4), `frida_level2.py` / `level3_report.py` / `merge_status.py` (need the Frida live tool, not published here),
`hang_dump.py`, `pdb_sym.py`, `civ_watchdog.ps1` (dump a hung game, resolve DLL addresses with the PDB).
