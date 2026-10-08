# Testing Dev CE (1,033 functions)

This repository has **1,033** generated bridge entries on 43 Lua objects (the released v0.1.0 has 251). The extra ~780 come from eight "sets" in `dev-ce/data/sets/`
(see `SETS.md` there: influence, diplomacy, culture/religion/stats, city, player/economy, world, unit, eras). **Status: plumbing-verified, behavior mostly unverified.** All of them load, are called
with the right arguments and returned values, and 115 mutators were executed for real without a crash (see `dev-ce/RESULTS.md`). What a function does in the game is known for only a small part of them.
Names and signatures come from debug symbols of another platform. If you want to help, test the effect of functions that interest you (single player, disposable game) and report what you see.

## What you need
* Windows, Steam Civilization VI with Gathering Storm (build 15038592 or 15296837, same GameCore DLL; the bridge checks this at startup and switches itself off on anything else).
* Visual Studio 2022 Build Tools (x64, C++), plus the capstone and MinHook libraries on the linker path as for the upstream
  [Community Extension](https://github.com/Wild-W/CivilizationVI_CommunityExtension) (its wiki describes the build). The generated table `DevNativeTable.cpp` is committed, so you do **not** need the Python tools or any analysis data to build.
* Build the project, x64 Release, `CL=/DCAPSTONE_STATIC`; the output is `GameCore_XP2_CE_FinalRelease.dll`.

## Install
1. Copy the folder `dev-ce/mod/DevCE_Test` to `Documents/My Games/Sid Meier's Civilization VI/Mods/`.
2. Create `DevCE_Test/Binaries/Win64/` inside it and put the DLL there.
3. Enable **Dev CE test build** in Additional Content. Disable every other mod that replaces the GameCore (the Community Extension, `DevCE`, other DLL mods).
4. Start a **new single-player game**, found a city, end two turns. Do not use a save you care about.

## What happens and what to send back
The test mod runs level-1 checks and the "oracle" getter comparison by itself at the start of your first turns.
* `Mods/DevCE_Test/Binaries/Win64/DevBridge.log`: startup line (DLL version, build stamp, how many functions are ready or disabled), then one line per interface.
* `Logs/Lua.log`: lines containing `DevCE` (present / missing methods per object, oracle OK / MISMATCH counts).

Please send both files and say whether the game started, whether it crashed or hung, and where. If it hangs, create an empty file `DevBridge.trace` next to the DLL, start again, and send the log: the last `[trace]` line names the call that never came back.

Level 2 and level 3 of the test plan (calling every function with recording stubs, then for real) need a Frida-based driver that is not published, so they are not part of this round.
If you want to call individual functions yourself (single player, disposable game), they are ordinary methods on the existing objects, for example `Players[0]:GetInfluence():SetTokensOnFirstToMeet(1)`;
the list is `dev-ce/data/exposed.json`, the sets are `dev-ce/data/sets/*.txt`.

## Known risks
* The earlier build froze a whole PC three times because of one wrong argument type in one function (fixed since), and `Unit.ChangeSightRange` hung a late-game save. A different untested entry can do the same: **save nothing important and keep other programs saved.**
* Arguments are not range-checked; a wrong id can crash the game. A guard converts access violations into a Lua error and a `FAULT` line, but not silent memory corruption.
* Many functions change game state directly and can desync multiplayer. Single player only.
