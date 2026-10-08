# Dev CE (experimental)

Dev CE is a fork of [Wild-W's Civilization VI Community Extension](https://github.com/Wild-W/CivilizationVI_CommunityExtension) (CE). It adds **1033 engine functions to 43 existing Lua objects**
(`City`, `Unit`, `Player`, `Game`, `Map`, ...). The functions come from a generated table (`DevNativeTable.cpp`, made by `dev-ce/tools/gen_bridge.py`) and go through one generic, tested dispatcher (`DevBridge.cpp`),
instead of one hand-written wrapper each. Everything CE does is still there. AGPL-3.0, like CE.

**Looking up a function?** The [GameCore reference](https://cru121.github.io/civ6-gamecore-reference/) has a [Dev CE section](https://cru121.github.io/civ6-gamecore-reference/devce/) with one page per Lua object:
every exposed method with its engine signature, argument and return meaning, test status and an (AI-written, flagged inferred) summary of what the engine function does. The same reference documents the vanilla Lua API,
the engine classes behind it and the address mapping this fork uses ([`data/`](https://github.com/cru121/civ6-gamecore-reference/tree/main/data)).

## Install
**This branch (1033 functions) has no release zip yet** (the repository contains no binaries). Build the DLL yourself (see *For contributors* at the end, about one minute with Visual Studio 2022 Build Tools) or use the older
[v0.1.0 release](https://github.com/cru121/civ6-dev-ce/releases) (251 functions, `DevCE-0.1.0-experimental.zip`; check the SHA-256 in the release notes).
1. Copy the folder `dev-ce/mod/DevCE` (or `DevCE_Test`, see *The mods*) to `Documents/My Games/Sid Meier's Civilization VI/Mods/`, create `Binaries/Win64/` inside it and put the DLL there. Optional: also copy `dev-ce/mod/DevCE_Playground` (the demo button panel, see below).
2. Start the game, open **Additional Content** and enable **Dev CE (experimental)** (and **Dev CE Playground** if you copied it). Disable every other mod that replaces the GameCore (the Community Extension, other DLL mods).
3. Start a **new single-player game**. `Mods/DevCE/Binaries/Win64/DevBridge.log` should now exist, and `Lua.log` should contain `DevCE: player mod loaded; Dev CE methods PRESENT` after the first turn.
Windows and the Steam version of the game only. Your antivirus may warn about the DLL because it replaces the game's GameCore, as the Community Extension does; the source is in this repository.

## Read this first
* **Experimental. Single player only.** Many functions change game state directly and can desync multiplayer. There is no multiplayer guard.
* **Steam builds 15038592 and 15296837** (both ship the same GameCore DLL; its PE timestamp and image size are checked at startup, and on any other GameCore the bridge disables itself and says so in `DevBridge.log`). Tested in game on 15038592 only. **Status: plumbing-verified, behavior mostly unverified** (see *What was tested*): the functions exist, are called correctly and do not crash in the tests below, but for most of them nobody has checked what they do in the game.
* **Do not save a game you care about while testing.** Some functions have lasting effects, and those effects are written into the save (tested for era score, influence tokens, alliance points and gold rate).
* **Potentially unsafe functions:** `Unit.ChangeSightRange` (hung the game once on a late-game save),
  `PlayerTrade.ChangeDomesticTradeDisabledCount`, `ChangeInternationalMajorsTradeDisabledCount`, `ChangeInternationalMinorsTradeDisabledCount` (+1 disables trade routes, -1 does not bring them back).
* **Arguments are not range-checked.** An out-of-range id or index can crash the engine. A guard turns hardware faults (access violations) into a Lua error and a `FAULT` line in `DevBridge.log`,
  but it cannot catch silent memory corruption. 502 of the 1033 functions return nothing, so you will not see whether they worked unless you read the state some other way.
* Only **one** GameCore-replacing mod may be enabled (Dev CE, Dev CE test build, the Community Extension, ... never two).

## Try it: Dev CE Playground
[`dev-ce/mod/DevCE_Playground`](dev-ce/mod/DevCE_Playground) is a small demo mod with an always-visible button panel (right edge of the screen). Enable it together with `DevCE` or `DevCE_Test` (the Dev CE buttons report that Dev CE is missing otherwise). Buttons: Golden / Dark Age on and off for yourself (`GameEras:SetGoldenAge` / `SetDarkAge`),
Era +1 / -1 (`GameEras:SetCurrentEra`, also reaches eras added by other mods), spawn James of St. George (vanilla), action charges of the selected unit (vanilla `Unit:ChangeActionCharges` next to Dev CE's `UnitGreatPerson:ChangeActionCharges`,
which changes the great person's *Actions* count that vanilla Lua cannot reach), capital food +/-2 and trade route capacity +/-1. Each press reports before/after values in the panel and in `Lua.log`.
What the demo showed: the era and age functions are raw setters (no era-change popup, no dedication choice, no era score, and Golden and Dark can both be on at once); the charges, food and trade functions do exactly what they say and reverse cleanly.
A second, collapsible column has sample buttons for the newer sets (influence tokens, alliance points, tourists, civics, capital production, gold rate, era score, park charges, XP level) and an **R41** button that checks the Deal objects while a negotiation screen is open; `DevCE_Reach41_Gameplay.lua` checks the District, Territory, Fallout and Free Cities objects by itself at the first turn.
It changes your game directly: single player, disposable games only.

## The mods
| mod folder | what it is |
|---|---|
| `dev-ce/mod/DevCE` | the bridge: call the new methods from your own scripts. It only writes one line to `Lua.log` at the first turn (a mod needs at least one in-game action, otherwise the game does not use its GameCore DLL) |
| `dev-ce/mod/DevCE_Test` | the same DLL plus self-test scripts that run at the start of your first turns and write results to `Lua.log` (lines with `DevCE`) and `DevBridge.log`. Use this one when you want to check that it works on your machine, or when you report a bug |
| `dev-ce/mod/DevCE_Playground` | the demo button panel (needs `DevCE` or `DevCE_Test`) |

Only one of `DevCE` / `DevCE_Test` may be enabled. The repository contains no binaries: the DLL is in the release zip, or build it yourself (below).

## Troubleshooting and bug reports
Everything useful goes to **`DevBridge.log`, in `Binaries/Win64` next to the DLL** (inside the mod folder under `Documents/My Games/Sid Meier's Civilization VI/Mods/`). It records the DLL version, the GameCore build stamp,
how many functions are ready or disabled, one line per interface registered on every game load, processor diagnostics (`[GameProcessor]`), and a `FAULT` line (function, `this`, argument values) whenever the crash guard catches something.
For the test mod also include the `DevCE` lines from `Logs/Lua.log`.

If the game **hangs or crashes** with no useful line: create an empty file named `DevBridge.trace` next to the DLL (or call `DevCE_Trace(true)` from Lua) and start again. Every call into the bridge is then written to
`DevBridge.log` *before* it runs, so the last `[trace]` line names the call that never came back. Delete the file again afterwards, it makes the log large.

A bug report needs: what you did, `DevBridge.log`, the `DevCE` lines of `Lua.log`, and whether the game was a new game or a loaded save.

## Calling the functions
They are normal methods on the objects you already have, named like the engine functions (`city:ChangeYieldChange(YieldTypes.FOOD, 2)`, `Game.GetEras():SetCurrentEra(3)`). The full list with C++ names, signatures, argument and
return kinds is `dev-ce/data/exposed.json`; `dev-ce/data/function_status.json` says how far each one has been tested. Function summaries written by an AI from decompiled code are marked inferred in the docs.

## What was tested
Details and raw logs: [`dev-ce/RESULTS.md`](dev-ce/RESULTS.md). Tested in game on Steam build 15038592, single player, disposable games and a copy of a late-game save.
* **Startup address check 1033/1033**, no function disabled, no `FAULT` line in any run.
* **Present on the live objects: 1032 of 1033** (Game and Map entries included; the District, Territory, Fallout, Deal and route-portal objects are checked by the Playground's Reach41 script). The one left is `DealItem:GetParentType`, which needs a deal with an item in it.
* **Read-only getters:** 33 vanilla-equivalent getters give exactly the same value as the vanilla Lua getters (new game and a late-game save).
* **Argument plumbing (Frida stubs): 824 functions PASS, 0 FAIL**: each native function is entered once with `this`, every argument and the return value exactly as sent. The Player root, Game and Map entries are not stub-tested (the engine calls them from several threads).
* **Real execution: 115 `Change*` functions called for real (+1, then -1)** on a new game and a late-game save: no crash, 112 restored, 3 not (the trade functions above); **27 show a visible effect** through vanilla getters (food surplus, route capacity, amenities, build and action charges, ...), 88 are silent because no vanilla getter shows what they change.
* **Save and load:** era score, influence tokens, alliance points and gold rate changed by Dev CE survive save, quit to menu and load.
* `GetIO()` (see below) works from gameplay scripts.
* Earlier (251-function build): about 2,000 calls with wrong objects or garbage arguments all raised a clean Lua error; 5 extreme-value calls on one getter faulted and were caught by the guard.
* **Not tested:** what most of the ~900 other functions actually do (they are plumbing-verified only), hostile arguments on the newer sets, multiplayer, loading a save made after a mutator ran in plain CE or vanilla, other game builds, other machines.

## Extra: `GetIO()`
The game does not open Lua's `io` library. `GetIO()` returns it: `local io = GetIO(); local f = io.open("test.txt", "w"); f:write("hi"); f:close()`. Relative paths land in the folder of `CivilizationVI.exe`. **It gives any Lua script read and write access to files the game process can reach**, so only enable mods you trust. Tested from gameplay scripts only (not from UI scripts).

## Credits, licence and provenance
* **Based on the [Community Extension](https://github.com/Wild-W/CivilizationVI_CommunityExtension) by Wild-W** (see its [wiki](https://github.com/Wild-W/CivilizationVI_CommunityExtension/wiki) for what CE itself adds). Thanks for making it open source.
* **AGPL-3.0**, like the Community Extension this is forked from (`LICENSE.txt`). If you pass on a built DLL, link this repo so people can get the source. The git history shows what changed compared to Wild-W's version.
* **Where the names and addresses come from:** debug symbols that shipped with an older build of the game (the method from the Community Extension contributor's guide), matched to the current Steam build 15038592 (identical DLL in 15296837) by comparing code bytes and call graphs.
  Struct layouts were cross-checked against the Linux port's debug info. That is why the addresses only work on that build. The mapping is in [civ6-gamecore-reference](https://github.com/cru121/civ6-gamecore-reference) (`data/`).
* **What isn't here:** game binaries, assets, scripts or decompiled code. The generated table has function names, addresses, a few prologue bytes (to spot a wrong build) and argument kinds.
  Descriptions of what the functions do are written by an AI assistant from decompiled code and marked inferred.
* Unofficial fan project, not affiliated with or endorsed by Firaxis, 2K or Take-Two, and not endorsed by the Community Extension's author. Please don't use it to cheat in multiplayer.

## For contributors
Build: Visual Studio 2022 Build Tools, x64 Release, project `CivilizationVI_CommunityExtension.vcxproj`, capstone and MinHook on the linker path (`CL=/DCAPSTONE_STATIC`); output name `GameCore_XP2_CE_FinalRelease.dll`.
(`asmjit/x86` is un-ignored in `.gitignore`: the original ignore rule `x86/` drops it from a clone.)
Tools are in `dev-ce/tools`: `gen_bridge.py` (table generator; `--scope gap --include <set>` builds the themed function sets in `dev-ce/data/sets`), `make_sets.py` (splits the candidates into sets), `gen_selftest.py` (test mod generator), `l2chunk.py` (level-2 test chunks), `hang_dump.py`, `pdb_sym.py` and `civ_watchdog.ps1` (dump a hung game, resolve DLL addresses with the PDB).
The Frida test drivers there need a live-channel tool that isn't published here.
