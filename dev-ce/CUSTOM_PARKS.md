# Custom-shaped National Parks and park rules (experimental)

## Lua API (gameplay state unless noted)
- `NationalParks.DesignateCustom(playerID, {plotIndex, ...})` creates a park on 1 to 32 arbitrary plots. It checks only what keeps the game safe: every plot is on the
  map, no duplicates, none already in a park. **No tile rules are applied** (water, improved, foreign-owned tiles, any appeal are accepted); apply your own rules first.
  The first plot decides the owning city: tourism, amenities and removal with a city all follow plot #1, so it must be inside a city's borders.
  Returns 0, or an error code: 1 bad count (1..32), 2 outside map, 3 duplicate, 4 already in a park, 5 the game could not create its temporary record (map edge), 6 internal,
  7 the first plot is not inside a city's borders.
- `NationalParks.IsValidParkTile(player, plotIndex)` is the park rule currently in force (the game's own, or the rule set below); `IsValidParkTileEx(player, plotIndex, flags)` is the vanilla
  rule with flag 1 = ignore the district check and flag 2 = ignore the terrain/appeal requirement. Both exist in the UI Lua state too (read-only), as does `GetRules()`.
- `NationalParks.SetRules(minTiles, maxTiles, minAppeal, maxAppeal, flags, {districtIndex, ...} [, {wonderBuildingIndex, ...}])` activates a native rule set (flags: 1 tiles with an improvement are allowed, 2 mountains always fit, 4 natural wonders always fit).
  While active it **replaces the game's tile rule everywhere** (UI highlight, the vanilla Designate National Park action, the AI): a tile needs to be owned by the player, have no improvement (unless flag 1),
  not be in a park, have appeal in `[minAppeal, maxAppeal]` (plain mountain / natural wonder tiles are exempt if their flag is set), and either have no district, or a district from the list, or a world wonder from the wonder list (7th argument; a wonder tile is decided by that list, the wonder is `Plot:GetWonderType()`).
  Cleared at every game load. `ClearRules()`, `GetRules()` -> active, minTiles, maxTiles, minAppeal, maxAppeal, flags (vanilla values when inactive). Min/max tiles are for callers such as the picker; `DesignateCustom` does not enforce them.
- Optional Lua override: `RegisterProcessor("NationalParkTileValid", function(map) ... end)` with `map.PlotIndex`, `map.PlayerId`, `map.Valid` (0/1, the native answer); set `map.Valid` and `map.Handled = 1` and return a non-nil value to override. It runs only on the simulation thread
  holding the GameCore lock (AI and gameplay) and only for tiles owned by the player; the UI highlight, which runs on the UI thread, uses the native rules only, so it cannot see what only the processor decides. (Not tested in game yet.)

## The mod: Better National Parks (`mod/BetterNationalParks`)
One mod, needs the Dev CE GameCore mod (only one GameCore mod enabled); it replaces DevCE_ParkRules and DevCE_ParkPicker of 0.5.0.
- **Rules as data:** tables `NationalParkTypes(ParkType, MinTiles, MaxTiles, MinAppeal, MaxAppeal, AllowImprovements, AllowMountains, AllowNaturalWonders)`, `NationalPark_ValidDistricts(ParkType, DistrictType)` and `NationalPark_ValidWonders(ParkType, BuildingType)`
  (plain names, resolved at run time, so wonders/districts of late-loading content work; unknown names are skipped). A gameplay script reads the row `NATIONAL_PARK_DEFAULT` and calls `SetRules`. Defaults: 3..5 tiles, Preserve, six landmark wonders (Stonehenge, Hanging Gardens, Temple of Artemis,
  Machu Picchu, Mahabodhi Temple, Chichen Itza), appeal 2 or more. Wonder and district tiles must also pass the appeal rule. **The rules also change the vanilla action and the AI.**
- **Setup-screen options** (Advanced Setup, after the map seed): Nat. Parks: Custom Shapes (off by default; single player), Min/Max Tiles (1..32, only with custom shapes), Nat. Park: World Wonders (Landmark / All / None / Custom) with a Custom Wonder List,
  Nat. Park: Districts (Preserve / None / Custom) with a Custom District List. Anything not set (old saves) falls back to the table defaults.
- **Custom shapes (human player only):** a second button for Naturalists and Mountie-like units (any unit with `ParkCharges`); click tiles (green = pickable, yellow = picked, right-click = undo); the park is created at the maximum tile count, or click the last tile again once the minimum is reached.
  All tiles must belong to one city and touch each other. The vanilla action is not replaced.

## How it works
The exe draws park fences and the green border additively on each `NATIONAL_PARK_ADDED_TO_MAP` event, taking the plot list from the game state at that moment, so the list must be final before that single event. `DesignateCustom` suppresses the events while the game's
`DesignatePark` creates the record (any rhombus around a plot of the owning city), overwrites the plot list in place, appends further plots (growing the list with the game's own vector code), and sends one `RestoreVisualState`.
The rule hook replaces the game's `IsValidParkTile`, whose only caller is `GetPossibleParks`.

## Tested / not tested
Tested in game (build 26595377, single player): the rule tables and setup options reach the rules, vanilla action and custom button, parks of 1, 2, 5, 6 and 12 tiles, mountain, wonder and Preserve shapes, appeal gating of wonder and district tiles, a Mountie of a non-Canada player gets both buttons, end turn, save and reload.
Found and fixed during testing: a heap corruption when appending the sixth plot; wonders of late-loading content dropped by an SQL snapshot; HavokScript raises on `tonumber(nil)`/`tostring(nil)`.
Not tested: the AI with the new rules, the `NationalParkTileValid` processor, tourism numbers of big parks, tiles of several cities in one park, multiplayer, other builds.
