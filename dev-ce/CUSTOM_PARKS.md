# Custom-shaped National Parks (experimental)

New Lua (gameplay state): `NationalParks.DesignateCustom(playerID, {plotIndex, ...})` creates a park on 1 to 4 arbitrary plots
(returns 0, or an error code: 1 bad count, 2 outside map, 3 duplicate, 4 already in a park, 5 no plot can serve as base, 6 internal,
7 no plot owned by a city). `NationalParks.IsValidParkTile(player, plotIndex)` is the game's own rule; `IsValidParkTileEx(player, plotIndex, flags)`
is the same rule with flag 1 = ignore the district check and flag 2 = ignore the terrain/appeal requirement. Both validity functions are
also available in the UI Lua state (read-only).

How it works: the exe draws park fences and the green border additively on each `NATIONAL_PARK_ADDED_TO_MAP` event, taking the plot list from the
game state at that moment, so the list must be final before that single event. `DesignateCustom` suppresses the events while the game's
`DesignatePark` creates the record, overwrites the plot list in place, then sends one `RestoreVisualState`. `DesignatePark` crashes when
the base plot has no owning city; the function only uses city-owned plots as base.

`mod/DevCE_ParkPicker` is a sample UI mod: a second Naturalist button, pick four touching tiles of one city (green = pickable, yellow = picked,
right-click = undo). Mountains, tiles next to the Naturalist, world wonders (optional whitelist) and the Preserve district are allowed; see the
policy block at the top of its scripts. The vanilla action and the AI are untouched. Single player only; not tested with save/load of
custom parks beyond a quick check, nor with multiplayer.
