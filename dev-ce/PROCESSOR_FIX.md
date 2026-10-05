# Prototype for Community Extension issue #5 (RegisterProcessor race)

Status: **run in the game (2026-10-04): native congress hooks reach Lua handlers through the game's dispatcher, `ProcessorTest` round trip 41 -> 42 works.** The `Handled` convention below is built but not yet re-tested.
Background and evidence: `../findings/topics/lua-event-dispatch.md`.

## What changed (against upstream `master` e59b5aa; `src/` is a copy, the pristine clone stays in `../upstream`)
- `EventSystems.cpp/.h` rewritten, same public functions (`DoesProcessorExist`, `CallCustomProcessor`, `lRegisterProcessor`), so `AI.cpp` is untouched.
  - `RegisterProcessor(name, fn)` = `GameEvents[name].Add(fn)` in the calling Lua state, plus remembering the name. No `lua_State*` and no registry reference is stored.
  - `CallCustomProcessor` no longer runs Lua itself: it calls `GameProcessor::Call`.
  - Fixed on the way: master's `DoesProcessorExist` fell off the end without `return true` (undefined behaviour).
- New `GameProcessor.cpp/.h`: builds a game `Data::TypedVariantMap` (default typed manager, keys registered with `VariantManager::RegisterKey`), calls
  `GameCore::Lua::Utility::CallProcessor` (rva 0x24530), reads integers back. Refuses to run unless the calling thread holds the GameCore lock
  (`IEngineUtility::HasGameCoreLock`), which every dispatch we observed did.
- `Main.cpp`: `GameProcessor::Create()` and a test global `ProcessorTest(name, key, value) -> handled, value`.
- `GameProcessor.cpp` header lists every offset (installed build 15038592) with the symbol name.

## Behaviour differences to know about
- `CallProcessor` reports only that **a handler ran** (verified in game: a handler returning nothing still gives handled = true; no handler gives false); it does not expose what the handler returned.
  CE's old convention "handler returns true to override" therefore cannot be kept: `AI.cpp` now adds an integer `Handled = 0` to the map and a handler claims the decision with `map.Handled = 1`.
- Only integer map entries are transferred (all the existing congress choosers use ints). Floats/strings need more variant types.
- Handlers must be registered in the **gameplay** Lua state (that is where `GameEvents` lives).

## Build / test
- Build: `..\deps\build_ce.bat C:\stuff\claude\DLL\ce_issue5\src C:\stuff\claude\DLL\ce_issue5\build`, then copy the DLL into `mod\CE_Proc_Test\Binaries\Win64`.
- Mod: `mod/CE_Proc_Test` (only ONE GameCore-replacing mod may be enabled). Start a NEW single-player game, end 3 turns, read `Lua.log` for `CE_Proc`.
  Expected: `test 1 ... handled=true value=42`, `test 2 ... handled=false value=5`, `test 3 ... handled=false value=7`, then (once congress runs with AI) lines like
  `DistrictTargetChooser called (#1): OutcomeType=... PlayerId=...` which prove the AI hook path.
- Failure signs: no `CE_Proc` lines (DLL not loaded), `GameProcessor: refusing ... GameCore lock` in the CE console, or a crash right at turn 1 (map/variant layout wrong).
