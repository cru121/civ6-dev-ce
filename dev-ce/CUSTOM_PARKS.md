# Custom-shaped National Parks and park rules (experimental)

## Lua API (gameplay state unless noted)
- `NationalParks.DesignateCustom(playerID, {plotIndex, ...})` creates a park on 1 to 32 arbitrary plots. It checks only what keeps the game safe: every plot is on the
  map, no duplicates, none already in a park. **No tile rules are applied** (water, improved, foreign-owned tiles, any appeal are accepted); apply your own rules first.
  The first plot decides the owning city: tourism, amenities and removal with a city all follow plot #1, so it must be inside a city's borders.
  Returns 0, or an error code: 1 bad count (1..32), 2 outside map, 3 duplicate, 4 already in a park, 5 the game could not create its temporary record (map edge), 6 internal,
  7 the first plot is not inside a city's borders.
- `NationalParks.IsValidParkTile(player, plotIndex)` is the park rule currently in force (the game's own, or the rule set below); `IsValidParkTileEx(player, plotIndex, flags)` is the vanilla
  rule with flag 1 = ignore the district check and flag 2 = ignore the terrain/appeal requirement. Both exist in the UI Lua state too (read-only), as does `GetRules()`.
- `NationalParks.SetRules(minTiles, maxTiles, minAppeal, maxAppeal, flags, {districtIndex, ...})` activates a native rule set (flags: 1 tiles with an improvement are allowed, 2 mountains always fit, 4 natural wonders always fit).
  While active it **replaces the game's tile rule everywhere** (UI highlight, the vanilla Designate National Park action, the AI): a tile needs to be owned by the player, have no improvement (unless flag 1),
  not be in a park, and either have no district and mountain/natural wonder/appeal in `[minAppeal, maxAppeal]`, or have a district from the list (an allowed district skips the appeal check).
  Cleared at every game load. `ClearRules()`, `GetRules()` -> active, minTiles, maxTiles, minAppeal, maxAppeal, flags (vanilla values when inactive). Min/max tiles are for callers such as the picker; `DesignateCustom` does not enforce them.
- Optional Lua override: `RegisterProcessor("NationalParkTileValid", function(map) ... end)` with `map.PlotIndex`, `map.PlayerId`, `map.Valid` (0/1, the native answer); set `map.Valid` and `map.Handled = 1` and return a non-nil value to override. It runs only on the simulation thread
  holding the GameCore lock (AI and gameplay) and only for tiles owned by the player; the UI highlight, which runs on the UI thread, uses the native rules only, so it cannot see what only the processor decides. (Not tested in game yet.)

## Mods
- `mod/DevCE_ParkRules`: tables `NationalParkTypes(ParkType, MinTiles, MaxTiles, MinAppeal, MaxAppeal, AllowImprovements, AllowMountains, AllowNaturalWonders)` and `NationalPark_ValidDistricts(ParkType, DistrictType)`; a gameplay script reads the row `NATIONAL_PARK_DEFAULT`
  (only that park type is read in this version) and calls `SetRules`. Defaults reproduce the vanilla rule. Other mods list it as a dependency and INSERT/UPDATE/DELETE rows in their own SQL.
- `mod/DevCE_ParkPicker`: a second Naturalist button; click tiles (green = pickable, yellow = picked, right-click = undo); the park is created when the maximum tile count is reached, or click the last tile again once the minimum is reached. All tiles must belong to
  one city and touch each other. Its SQL adds `DISTRICT_WONDER` and `DISTRICT_PRESERVE` to the valid districts, so world wonders and the Preserve district become part of the rule: **this also changes the vanilla action and the AI**; delete the rows in your own SQL to undo it.
  The vanilla action is not replaced. Single player only.

## How it works
The exe draws park fences and the green border additively on each `NATIONAL_PARK_ADDED_TO_MAP` event, taking the plot list from the game state at that moment, so the list must be final before that single event. `DesignateCustom` suppresses the events while the game's
`DesignatePark` creates the record (any rhombus around a plot of the owning city), overwrites the plot list in place, appends further plots (growing the list with the game's own vector code), and sends one `RestoreVisualState`.
The rule hook replaces the game's `IsValidParkTile`, whose only caller is `GetPossibleParks`.

## Tested / not tested
Tested in game (build 15038592, single player): rule tables read, vanilla action and custom button with the rules, parks of 2, 5, 6 and 12 tiles, mountain and wonder shapes, end turn, save and reload with a custom park. Found and fixed during testing: a crash (heap corruption) when appending the sixth plot.
Not tested: the AI with the new rules, the `NationalParkTileValid` processor, tourism numbers of big parks, tiles of several cities in one park, multiplayer, other builds.
