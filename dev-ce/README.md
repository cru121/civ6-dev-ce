# Dev CE (experimental)

A fork of [WildW's Civilization VI Community Extension](https://github.com/Wild-W/CivilizationVI_CommunityExtension) that exposes **251 engine functions
on 29 existing Lua objects** (`City`, `Unit`, `Player`, `Game`, `Map`, ...) through one generic, generated bridge instead of hand-written wrappers.
Licence: **AGPL-3.0** (same as upstream). Not endorsed by WildW, Firaxis or 2K.

**Experimental. Single player on disposable saves only.** Most functions mutate game state and can desync multiplayer. Addresses are valid **only for Steam build 15038592**;
on any other GameCore build the bridge disables itself (PE timestamp/size check).

## What is here
* `DevBridge.cpp/.h` runtime dispatcher, `DevNativeTable.cpp` generated table (names, RVAs, signatures), `GameProcessor.*`/`EventSystems.*` the RegisterProcessor fix (see `dev-ce/PROCESSOR_FIX.md`).
* `dev-ce/tools` generator (`gen_bridge.py`), self-test generator, Frida level-2/3 test drivers; `dev-ce/frida/devce.js` is the Frida side (needs the Frida live framework, not yet published).
* `dev-ce/mod/DevCE_Test` mod (modinfo + generated Lua self-test; build the DLL and put it in `Binaries/Win64/`). `dev-ce/mod/CE_Proc_Test`: processor test.
* `dev-ce/RESULTS.md` every test run. `dev-ce/data/function_status.json` per-function test status.

## Test status (see RESULTS.md for detail)
Address check 251/251; level 1 (methods present) 226; oracle (bridge getter == vanilla getter) 23/23; level 2 (argument plumbing via Frida) 212 pass, 27 excluded, 12 need a live District/Deal/Territory;
level 3 (real +1/-1 calls) no crash, restored except `Unit.ChangeSightRange` (reveals tiles). 216 of the functions return nothing, so most "pass" means "did not crash and arguments arrived".
Not tested: multiplayer, save/load, unusual arguments. The latest additions (build check, SEH crash guard, log lock) are built but not yet run in game.

## Build
Visual Studio 2022 Build Tools, x64 Release, with capstone and MinHook on the linker path (`CL=/DCAPSTONE_STATIC`). Output name `GameCore_XP2_CE_FinalRelease.dll`.
Enable only **one** GameCore-replacing mod at a time.
