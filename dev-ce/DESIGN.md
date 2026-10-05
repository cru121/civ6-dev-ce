# Dev CE: exposing discovered engine functions to Lua (design draft, 2026-10-03)

## Goal
A development clone of the Community Extension (CE) DLL whose Lua additions are **generated from a small spec per function** instead of hand-written C++,
so that many discovered engine functions (the ~350 ranked candidates in `gap_shortlist.tsv`) can be exposed cheaply, tested automatically in the running game
and documented from the same source. Not for upstream yet (user decision: "not for a long time"); nothing here is a contribution.

## Pieces
```
spec/*.yaml      one entry per exposed function (hand-written, reviewed)
generator/       spec -> generated C++ (wrappers + registration), test manifest, reference-doc entries
runtime/         fork of the CE source tree (hooks RegisterMembers of an object type, pushes extra Lua methods) - already builds (ce_poc)
tests/           manifest-driven checks run through frida/live (hook + call, snapshots)
docs/            generated into docs_proto as availability "needs dev CE"
```

## Spec entry (draft)
```yaml
- lua: Governors.UnassignGovernor          # object.method as seen by modders
  native: Player::Governors::UnassignGovernor   # symbol in old_to_new_offsets.tsv; the generator resolves the installed-build RVA
  this: PlayerGovernors                    # how to get `this` from Lua arg 1 (an existing game GetInstance wrapper)
  params: [ {name: governor, type: GovernorInstance}, {name: a, type: bool}, {name: b, type: bool} ]
  returns: void
  class: mutator                           # getter | mutator | dangerous  (decides which tests run and what the docs warn)
  state: gameplay                          # register for the gameplay state only (UI state must not mutate)
  status: unread                           # unread | decompiled | tested-in-game | verified
```
Generated: `int lUnassignGovernor(lua_State*)` that reads the arguments (`hks::checkinteger`, `toboolean`, ...), gets `this`, calls the function through a typed pointer,
pushes the result; a registration line; a test manifest line; a docs entry.

## The hard parts (not solved yet)
1. **Instance resolution.** Each Lua object kind has its own `GetInstance(L, idx, bool)` template instantiation in the DLL. CE hand-binds a few
   (Unit, Plot, Player::Cities, Governors, Influence, GameDiplomacy). We have decompiled all 2,281 wrappers, so the table `object -> GetInstance rva` can be mined
   from them (same decoding as `lua_signatures.py` self-argument detection). Objects without a usable GetInstance need a hand-written resolver.
2. **Argument types.** Linux DWARF gives parameter types/names for 21,782 functions. Simple types (int, bool, float, enums, ids) can be generated. Pointers to game
   objects (`Instance*`, `Plot*`) need instance resolvers; STL types and out-parameters need hand-written glue.
3. **Calling convention / layouts.** Windows x64 is a single convention, but struct layouts differ from Linux DWARF by -8 for FAutoVariable-heavy classes
   (see `findings/topics/linux-debug-symbols.md`). The generator must never take offsets from DWARF without the Windows check.
4. **Multiplayer / sync.** Mutators change simulation state; they must run on every client in the gameplay state or desync. Policy: mutators are registered only
   in the gameplay state, and docs say they are unsafe in multiplayer until tested.
5. **Build drift.** Spec uses symbol names, so a new game build only needs a new old->new map, not new RVAs by hand.

## Test levels (run through frida/live, the game is the oracle)
* **L0 address**: prologue of the resolved RVA matches the symbol build bytes (masked) - already how the map is validated.
* **L1 reach**: hook the native function, run the Lua method from a test script, assert the function was entered once with the arguments Lua passed.
  Catches wrong RVA, wrong `this`, wrong argument marshaling.
* **L2 effect**: snapshot related state with existing getters (or the Frida readers), call, diff. Needs a fixture save and per-function expectations.
* **L3 persistence/sync**: save/reload, and (later) a two-client session. Manual.
Getters get an automatic L1 + type check; mutators get L0 + L1 automatically, L2 by hand-written expectations.

## Phases
1. Spec format + generator for simple this-method calls on classes that already have an instance resolver in CE (Unit, Plot, Player::Cities, Governors, Influence). Regenerate the existing CE_PoC `ChangeBuildCharges` from a spec and prove the generated DLL behaves the same (L1 test).
2. Mine `GetInstance` rvas for all objects; widen to ~40 low-risk functions (getters first, then simple mutators from the shortlist).
3. Test runner: manifest -> frida/live -> report; fixture save.
4. Docs: spec -> docs_proto entities (availability "dev CE"), runtime-verified marks.
5. Only then: decide about packaging/publishing, license (CE is AGPL: a fork must stay AGPL), provenance notes for the leaked-symbol-derived addresses.

## Open decisions for the user
* Namespace for new methods: put them on the existing Lua objects (like CE) or under a separate prefix to avoid clashes with upstream CE later?
* Target audience of the first public release: modders wanting new calls, or reverse-engineers wanting a research build? (Changes what "tested" must mean.)
* Which first batch: from the shortlist (events/notifications fire as a side effect, riskier) or pure getters/setters of single fields (safer, less exciting)?
