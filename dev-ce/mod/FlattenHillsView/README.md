# Flatten Hills View (UI-only mod)

When a script turns a **hills** tile into flat land (`TerrainBuilder.SetTerrainType`), the game data changes but the 3D map keeps drawing the hill until the game is
reloaded. This mod flattens the hill on the map immediately, with the engine's own heightmap edit, and updates the yield icons.

## What it does
* A tile that **was a hill when the game was loaded** and is now flat land (grass, plains, desert, tundra, snow) gets the engine landmark `LM_HEX_FLATTEN`
  (`AssetPreview.SpoofLandmarkAt`, the same call the FireTuner asset preview uses). The mesh, the ground height units walk on and the sampler follow.
* If the tile becomes hills again the edit is removed (`AssetPreview.ClearLandmarkAt`) and the real, rugged hill is back exactly.
* After **any** terrain change the yield icons ('Show Yield Icons') of the changed tiles are refreshed (the game fills that layer once at load).
* Works in the fog of war and after quick load (the baseline is taken again when a game is loaded). Nothing is saved: a load rebuilds the map from the game data.

## What it does not do (still needs a reload)
* flat -> hills, mountains, coast / ocean (no engine edit exists for them; the Dev CE terrain view, `dev-ce/TERRAIN_VIEW.md`, does flat -> hills),
* the ground texture of the flattened tile (it keeps the hills texture),
* features, resources and improvements standing on the tile keep their old height (the Dev CE terrain view re-places them with the game's builders; a pure UI mod cannot).

## Files
`FlattenHillsView.modinfo`, `UI/FlattenHillsView.xml` (hidden status label + context), `UI/FlattenHillsView.lua`. No gameplay script, no Dev CE needed, affects no saved game.

## Debugging
Civ VI's Lua log is empty by default. The mod keeps its last 12 log lines in the hidden label `/InGame/FlattenHillsView/Fhv_Status`; from another UI Lua state:
`ContextPtr:LookUpControl("/InGame/FlattenHillsView/Fhv_Status"):GetText()` (a test script can read it).
