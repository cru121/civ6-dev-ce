# Dev CE bridge: test results (running log)

## 2026-10-03: first milestone, build 15038592, single player, disposable game
Bridge: generic dispatcher + generated table, 61 functions on 18 Lua objects (from the 350 ranked candidates).
* Address check at startup (first 8 bytes of every function and GetInstance): 61 of 61 ready, 0 disabled.
* Level 1 (generated Lua self-test, reflection): 53 methods present, 0 missing; 8 not reached (no live Deal/District/AreaPortal/Territory instance).
* Level 2 (plumbing, Frida stubs recording inside a gameplay-script window with a handshake): 53 PASS, 0 FAIL, 8 not run. For each: native function entered exactly
  once, `this` non-null, every sentinel argument (ints, uints, int64, bools as 1) received exactly, return value round-trips to Lua (void -> nil, bool, int, uint).
  Raw: data/level2_results.json. NOT tested: that the functions do what their names/agent summaries say (real execution), multiplayer, save/load.
* Found by review: one exposed "method" was a constructor (City::BuildQueue::BuildQueue). The generator now never exposes constructors, destructors or operators.
* Technical findings: gameplay Lua runs on a worker thread, one short-lived coroutine state per callback (never run Lua in a recorded copy of it); RegisterScriptData's
  argument is not a usable lua_State; only one GameCore-replacing mod may be enabled.

## 2026-10-03 (second run): 162 entries on 25 Lua objects (138 candidates + 24 oracle getters), second `this` kind (ScopedVirtualInstance)
* Address check: 162 of 162 ready.
* Level 1 (reflection): 151 methods present, 0 missing; 11 not reached (no live Deal/District/AreaPortal/Territory).
* ORACLE (vanilla getter vs bridge-exposed engine getter of the same class, compared in the running game): **21 match, 0 mismatch, 0 error**. Covers plain and virtual
  interfaces (IUnit, ICityBuildings, IUnitExperience, IPlayerTechs, IGameGreatPeople, ICityDistricts, ICityGrowth, IPlayerReligion, IPlayerTreasury, and virtual IPlayerDiplomacy, IPlayerCulture).
  So `this` resolution and integer/bool marshaling are correct for those interfaces. No oracle available (rely on same mechanism): IGameEras, ICity, IPlayerTrade, IPlayerResources, IGameDiplomacy and others without a suitable vanilla getter.
* Level 2 (Frida stubs): **151 PASS, 0 FAIL**, 11 not run (no instance).

## 2026-10-03 (third run): level 3, delta probe with real execution (disposable single-player game)
65 reversible Change* functions (integer args, agent risk != high, confidence != low, no hash params). Each: snapshot ~440 vanilla zero-arg getters, call with +1, snapshot, call with -1, snapshot.
* **No crash, no Lua error, 0 call failures** in 130 real calls. **All 65 restored** by the -1 call (every getter back to its starting value).
* **8 with a visible effect through vanilla getters, all as the names say:**
  Unit.ChangeBuildCharges (GetBuildCharges 0->1), UnitGreatPerson.ChangeActionCharges (GetActionCharges 0->1), Unit.ChangeDisasterCharges (GetDisasterCharges 0->1),
  PlayerTrade.ChangeOutgoingRouteCapacity (GetOutgoingRouteCapacity 0->1), City.ChangeYieldChange(food,+1) (GetFoodSurplus 2->3, GetTurnsUntilGrowth 7->5),
  City.ChangeYieldModifier / ChangeFollowerYieldModifier (food surplus 2 -> 2.039), PlayerResources.ChangeExtraAmenitiesPerOwnedBonusResource (GetAmenities 2->3, GetAmenitiesFromLuxuries 0->1).
* 57 with no visible effect through vanilla getters: no vanilla getter exposes that state (modifiers, era score components, trade restrictions...) or the effect needs a condition
  that did not hold in this game. NOT evidence of failure; they need purpose-built checks.
* Raw: data/level3_results.json (report: tools/level3_report.py); merged status per function: data/function_status.json.

## 2026-10-03 (fourth run): 201 entries, fixed-point support with the MSVC x64 class ABI
* A first build with FixedPoint support CRASHED the game at the oracle step: MSVC x64 returns a non-aggregate class (FixedPointT<8>) through a hidden buffer in the slot after `this`
  and passes it by value as a pointer to a temporary; the bridge had put an argument in that slot (0) and the callee wrote to address 0. Fixed in the dispatcher (slots: this, hidden return buffer, arguments;
  FixedPoint argument = pointer to a local int32).
* After the fix: 201 of 201 address checks; level 1: 186 present, 0 missing, 15 not reached; oracle: PlayerCulture.GetCultureYield returns 1.296875 through the bridge = identical to the vanilla Lua getter
  (proves hidden return buffer + raw/256 conversion); level 2 (Frida stubs, dereferencing pointer arguments and writing the hidden buffer): **186 PASS, 0 FAIL**, 15 not run (no District/Deal/AreaPortal/Territory
  instance; PlayerStats has no accessor in the test script yet).

### Level 3 on the larger set (same run, 90 functions: 65 integer + 25 FixedPoint Change*)
* **No crash, 0 call errors** in 180 real calls (+1 and -1). The 25 FixedPoint functions were called for real with FixedPoint arguments passed by pointer to a temporary copy (the MSVC x64 rule): no fault.
* 89 of 90 restored by the -1 call. 8 with a visible effect, same as before (BuildCharges, ActionCharges, DisasterCharges, OutgoingRouteCapacity, City.ChangeYieldChange food, the two City yield modifiers).
* **Unit.ChangeSightRange(+1) is NOT fully reversible** (explained by the player who ran the test): +1 revealed tiles and the game met a scientific city-state at once (free envoy): GetScienceYield 3.5 -> 4.5, GetTurnsLeft 7 -> 6. The meeting and the reveal stay after the -1 call. A legitimate consequence of the reveal, not a hidden defect; documented as a side effect.
  map tiles (discovery bonuses/eurekas); revealing is not undone when the range shrinks again. Treat it as a method with lasting side effects.
* 82 silent (no vanilla getter shows the state). For the FixedPoint ones this also means the *value* interpretation (raw = Lua number x 256) is not yet confirmed by a visible effect; only that the call convention is accepted.


## Owner navigation (2026-10-03)
Windows member offsets found offline by call-site voting in the installed DLL (callers of each member's functions compute `lea rcx,[owner+off]` before the shared
`FAutoVariable<T,Owner>::edit` (rva 0x72a920, returns wrapper+0x10), or pass owner+off directly for embedded members). Offsets = Linux DWARF offset minus 8, except City::Power (minus 16).
| member | owner (Lua object) | offset | mode |
|---|---|---|---|
| City::Trade | City::Instance (City) | 0xe78 | FAutoVariable |
| City::CulturalIdentity | City::Instance | 0x1630 | embedded |
| City::Power | City::Instance | 0x1838 | FAutoVariable |
| City::Culture | City::Instance | 0xb80 | FAutoVariable |
| City::Gold | City::Instance | 0x8a8 | FAutoVariable |
| City::Combat | City::Instance | 0xa10 | FAutoVariable |
| Unit::Espionage | Unit::Instance (Unit) | 0x8c8 | FAutoVariable |
| Unit::Archaeology | Unit::Instance | 0x888 | FAutoVariable |
| Trade::Graph | Trade::Manager (TradeManager) | 0xc0 | embedded |
| Barbarian::ClansManager | Barbarian::Manager (BarbarianTribes) | 0x1e8 | FAutoVariable |
Bridge: FnDesc.navOff/navMode, `this` = edit(owner+off) or owner+off. Generator exposes 224 entries (+23). Not yet tested in game (no oracle exists for these members: verification = level 2 plumbing + level 3 visible effects).

### Owner navigation test (2026-10-03, run nav-1)
Level 1: 224 addresses OK, 212 methods present, oracle 23/23. Level 2: 212 PASS (12 need District/Deal/AreaPortal/Territory instances). Level 3: all 23 new City/Unit/Trade/Barbarian-member functions ran with no crash and restored; effects are silent (they need trade routes, spies, etc. to show), so offsets are crash-safe but their effect is not yet confirmed.

## Player root and singletons (2026-10-03, built, NOT yet tested in game)
* Player root (ownerKind 1): the Lua `Player` object is a PlayerReference {int id}; this = Context::Globals::EditPlayer(id) (rva 0x44f00). Player::Instance functions use it directly; components are pointer members (mode 3): Espionage +0x6f8, Districts +0x6e8, Goody_Hut +0x730, CulturalIdentity +0x770, TurnManager +0x788 (offsets from call-site voting), or the game's static Get(PlayerTypes) (mode 4): Bonuses 0x2610b0, Congress 0x26c770. 14 functions.
* Singletons (ownerKind 2): `Game.X(args)` / `Map.X(args)` static tables, no object argument; this = the game's zero-argument accessor (Game::Instance::Edit, Game::Economic/Climate/Gossip Manager::Get, Game::Culture/Techs::Get, Emergency::Manager::Get, Player::Manager::Edit, Rules::Appeal/Economic/Espionage::Get, Map::Route::Manager::Edit on Game; Map::EditInstance, Map::Feature::Manager::Get, Context::Globals::EditPlayerVisibility on Map). 13 functions.
* Total now 251 entries. Test generator wraps Game/Map in DevStatic() so the o:Method() test form works.

### Incident 2026-10-03 13:00: whole-machine freeze during the level-2 window (251-function build)
Game hung at the start of the level-2 window (12:54), no crash dump, user had to reboot (clean restart logged at 13:00, no bugcheck). Most plausible cause: the Frida stub swallowed EVERY call of a stubbed function during the window, including calls made by the engine itself; the new set contained engine-hot functions (Game.IncrementGameStateLock/DecrementGameStateLock, Player.SetAlive, Player Manager ClearLists ...), so the game-state lock was left unbalanced. Fix: stubs now only intercept calls whose return address is inside the Dev CE DLL (frida/live/cmds/devce.js); engine calls always run the original. Not proven. Level 2/3 of the 251 build has NOT passed yet.

### Second freeze 2026-10-03 14:4x (after the return-address fix) - revised diagnosis
Level 2 was armed (stubs installed), the user ended a turn, the machine hung again (game not closable, reboot). The return-address filter did not help, so the cause is the stubs themselves: a Frida NativeCallback runs JavaScript for EVERY call of the hooked function from EVERY thread, even when it only forwards (stubon 0). With the 27 new functions (Player root + Game/Map singletons: GameStateLock increments/decrements, SetAlive, Gossip, ClearLists...) some are called by the engine all the time from several threads -> deadlock/starvation as soon as the turn is processed. The 224-function build had no such functions and passed. RULE: never Frida-stub functions that the engine may call in hot paths. frida_level2.py now never stubs functions reached through the Player root or the Game/Map singletons (hot()), the generated level-2 table skips them, and collect reports them as EXCLUDED. They stay exposed; their plumbing is checked by level 1 (present), real Change* execution in level 3, and community tests.

### FINAL diagnosis of the three freezes (2026-10-03) - supersedes the two incident notes above
All three were the SAME unhandled exception in the game (fullscreen crash dialog that cannot be closed), not stub deadlocks. Cause: a bridge bug. Player::Bonuses mode-4 accessor was 0x2610b0, which is the overload taking a Player::Instance POINTER (the engine's effect code calls it with the player object); the bridge passed the player ID, so the function dereferenced a small integer. The accessor runs for real even when the target function is stubbed (level 2), so level 2 crashed at the first Player.ChangeFlatBonus call (twice) and level 3 crashed at "BEGIN Player.ChangeFlatBonus" (DevBridge.log showed it; unit position and fog reverted visually = the crash). The id-taking overload is 0x2610c0 (movsxd rbx,ecx; Player::Manager::Edit()[id]); Player::Congress::Get 0x26c770 takes an int as well and was correct. Fixed in gen_bridge.py NAV. The Frida "never stub engine-hot functions" exclusion is kept as a precaution (hot() in frida_level2.py) but was NOT the cause. Lesson: accessors called by the bridge are real code even in a stubbed level-2 run; verify an accessor's argument type from its disassembly, not from the Linux signature (the function index mapped the wrong overload).

### Run player-singletons-1 (2026-10-03, after the Bonuses accessor fix)
Level 1: 251 addresses OK, 226 methods present (Game/Map 'missing' is a script limitation), oracle 23/23. Level 2: 212 PASS, 27 EXCLUDED (Player root + Game/Map singletons, deliberately never stubbed), 12 NOT_RUN (no instance). Level 3: no crash, no call errors, 113 restored by -1; the Player-root and Map Change* functions (ChangeFlatBonus, ChangeResolutionEffectRefundPercent, ChangeIdentityPerTurnForTradeRouteOrigin, ChangeSpyCapacity, ChangeYieldRate, ..., Map.ChangeNaturalWonderCount) ran and restored; effects silent via vanilla getters. Not run for real (destructive): Player.SetAlive, Game.Increment/DecrementGameStateLock, Game.ClearLists, SetLocalPlayerTo, BuildTechs/BuildCivics, Map.SetVolcano*, SetRevealed*, AddPlotYield, Player.AddAgenda, Add/RemoveChopFeatureBehavior, RecomputeCost.

## Processor fix ported from CE prototype (2026-10-05) - BUILT, NOT YET TESTED IN DEV CE
Ported from `../ce_issue5` (CE issue #5, `RegisterProcessor` race / dangling `lua_State*`): `GameProcessor.cpp/.h` (new), `EventSystems.cpp/.h` (rewritten),
`AI.cpp` (`Handled` key), `Main.cpp` (`GameProcessor::Create()`, test global `ProcessorTest`), project file entries. Same code as the prototype that was tested in
CE (single player incl. a World Congress; see `../ce_issue5/README.md`), so it is expected to work here, but nothing in Dev CE exercises it.
- `RegisterProcessor(name, fn)` = `GameEvents[name].Add(fn)`; native hooks call the game's `CallProcessor`. A handler claims an AI congress decision with `map.Handled = 1`
  (the game's dispatcher only reports that a handler ran). Only integer map entries are transferred.
- TODO / to test: (1) level 1 + oracle self-test still pass with this DLL (regression check; nothing else changed); (2) one run with `../ce_issue5/mod/CE_Proc_Test/Scripts/CE_Proc.lua`
  added as a gameplay script (expect `test 1 ... handled=true value=42`); (3) `GameProcessor.log` appears next to the game exe (`Base/Binaries/Win64Steam`) - diagnostic, capped at 60 lines per run, remove before publishing;
  (4) multiplayer and handlers that actually claim a decision are untested everywhere.
- Not deployed into the user's Mods folder yet (`tools/deploy_when_closed.sh`, and CE_Proc_Test is currently the enabled GameCore mod).

## Easy wins (2026-10-05) - BUILT, NOT YET TESTED IN GAME
* GameCore build check (PE timestamp 0x667c6f5b + SizeOfImage 0xc60000): on mismatch the whole bridge is disabled and logged.
* SEH guard around every native call (CallGuarded): a hardware fault becomes a Lua error + 'FAULT' line in DevBridge.log (FnState.faults). Cannot catch silent corruption.
* DevBridge::Log is now serialised with a critical section.
* TODO test: DevBridge.log must show the PE line and '251 native functions ready'; level 1/oracle unchanged; then the hostile-argument pass (nil, negative, huge, '.' vs ':') expecting Lua errors, no crash.

### 2026-10-05 first run of the easy-wins build (processor port, build check, SEH guard, log lock)
* First two runs HUNG at startup: my log-lock edit lacked LeaveCriticalSection (script edit silently not applied), the second thread to log blocked forever. Found with the Frida watchdog dump (tools/civ_watchdog.ps1, hang_dump.py, pdb_sym.py). Fixed.
* After the fix, new game: PE check OK, 251/251 ready, level 1: 226 present / 13 'missing' (known: Game/Map static tables not enumerable by the script) / 12 not reached; ORACLE 23 match, 0 mismatch; no FAULT lines, no Lua syntax errors. Processor fix not exercised here (CE_Proc_Test not enabled). NOT yet run: level 2/3, save/load, hostile arguments.

### 2026-10-05 late-game save (forum save AutoSave_0388 + Dev CE added by saves-cli), easy-wins build
* Save loads and plays with Dev CE; a turn ends normally. Level 1 226 present; oracle 23/23 on real late-game values (faith 608.7, building maintenance 789).
* Level 2 (Frida, 224 stubs): 212 PASS, 27 EXCLUDED, 12 NOT_RUN (District/Deal/Territory/AreaPortal: the self-test script does not find live instances even here -> fix the script accessors). No fault, game responsive.

### 2026-10-05 level 3 on the late-game save: HANG at Unit.ChangeSightRange
Level 3 ran about 100 functions fine (all `plus=true|minus=true`, restored), then the game stopped responding at `L3|BEGIN|Unit.ChangeSightRange` (no result line; no FAULT, so not a hardware fault).
The watchdog dump (game thread waiting inside the engine, Lua frames below it, no Dev CE frames) was taken after 20 s and the game was killed. In the earlier small game the same function ran (and revealed tiles, met a city state).
Cause unknown: the hang is either in the +1 call, in a getter of the snapshot that follows it, or in the engine's reveal handling on a big late-game map; not isolated. Unit.ChangeSightRange is now skipped by level 3 (L3_SKIP in gen_selftest.py) and should be treated as unsafe.
Log kept: data/devbridge_hang_sightrange.log. The remaining level-3 functions after it (rest of Unit.*) were not run in that pass.

### 2026-10-05 level 3 on the late-game save (without Unit.ChangeSightRange): 112 functions
* **No crash, 0 call errors**, 112 of 112 executed (+1 and -1). Late-game state (cities, trade routes, resources) makes far more effects visible: **27 with a visible effect through vanilla getters** (was 8 in the small games), all in the direction the names say: city yield changes (food surplus, turns until growth) for City/CityBuildings/PlayerTrade yield functions, PlayerTrade.ChangeOutgoingRouteCapacity 37->38, charges on Unit/UnitGreatPerson, PlayerResources.ChangeExtraAmenitiesPerOwnedLuxuryResource (faith/science/gold yields change). 85 silent.
* **3 NOT restored by -1: PlayerTrade.ChangeDomesticTradeDisabledCount, ChangeInternationalMajorsTradeDisabledCount, ChangeInternationalMinorsTradeDisabledCount.** +1 disables trade routes (gold 3980 -> 3057, faith -18, a city's food surplus 11 -> 6) and -1 does not bring them back, same class of lasting side effect as Unit.ChangeSightRange: the counter is restored but routes that were switched off / cancelled are not. Treat as destructive in the docs. The game used for this run must not be saved.
* Tools: dev-ce/tools/level3_report.py, merge_status.py --record late-game-1.

### 2026-10-05 level 4: hostile arguments (late-game save, easy-wins build with SEH guard)
* 239 functions, 2,263 logged calls, **no crash, no hang**, game responsive afterwards. Raw: data/devbridge_level4.log.
* Section A/C (invalid object, other object kind, missing/garbage arguments; 1,970 calls): **all raised a clean Lua error** (`Not a valid instance. Either the instance is NULL or you used '.' instead of ':'` or `bad argument #n (integer expected, got ...)`). **0 UNEXPECTED_OK**: no invalid object slipped through the type check, so a wrong 	his does not reach native code.
* Section B (extreme values on the 8 argument-taking read-only getters x 7 values): 51 returned a value (-1 or 0 results for out-of-range ids), **5 raised access violations that the SEH guard caught**: UnitExperience.HasPromotion with INT_MAX, INT_MIN, 1e10, 1e300 and NaN (an unchecked promotion index in the engine). Each became a Lua error plus a FAULT line in DevBridge.log (`Unit::Experience::HasPromotion`); the game kept running. So the guard works and an unchecked index in a getter is a real way to crash the game without it. Silent memory corruption cannot be excluded by this test; no mutator was called with extreme values on purpose.
* Lesson for docs: ids/indices passed to exposed functions are NOT range-checked by the bridge; the guard only catches hardware faults.

### 2026-10-05 processor fix (CE issue #5 port) tested inside Dev CE: PASS
* Late-game save (AutoSave_0388 + Dev CE), CE_Proc.lua added to the Dev CE test mod, two turns ended, game responsive, no Lua errors.
* test 1 (handler adds 1): handled=true value=42; test 2 (handler returns nothing): handled=true value=999; test 3 (no handler): handled=false value=7; all as expected, on both turns.
* The 11 congress target choosers hooked natively (AI.cpp) were called by the game (World Congress resolution in this save, several players, OutcomeType 1 and 2); the test handlers only log and do not set map.Handled, so the game's own chooser decided and the turn ended normally. GameProcessor.log: every call on one thread with the GameCore lock held (lock=1), sent=4, handled=1.
* Not tested: a handler that actually claims a decision (map.Handled = 1), multiplayer, save/load with handlers registered.

### 2026-10-05 save/load and menu round trip: PASS
New game -> save (RT1) -> quit to main menu -> load RT1 -> end turn -> new game from the menu (same game process, DLL never unloaded by hand). DevBridge.log shows one registration pass per game load (4 loads in the log, the first from the previous process): each identical: PE check OK, 251/251 functions ready, 26 interfaces / 240 methods added (no double hooking, nothing missing), UI cache states re-registered each time. After every load the Lua self-test ran again with the same result (226 present, 23/23 oracle matches, processor tests 42 / 999 / unhandled). No FAULT, no hang, game responsive.
Not tested: a save made AFTER calling a mutator (persistence of changed state), loading such a save in plain CE/vanilla.
