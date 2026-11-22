# What is in this zip

Copy the mod folders you want (not the grouping folders) into `Documents/My Games/Sid Meier's Civilization VI/Mods/`. Everything needs **Dev CE** except where noted. Install steps: `README-INSTALL.txt`.

## 1. `DevCE`: the main thing
The GameCore replacement (a fork of Wild-W's Community Extension): about 1000 engine functions for Lua, hooks, veto events, terrain view, Lua debugger, audio, National Park natives. It does nothing by itself; mods call it. Only one GameCore mod may be enabled.

## 2. `BetterNationalParks`
Custom-shaped National Parks, park rules as data, setup-screen options (see `CUSTOM_PARKS.md`). Needs Dev CE.

## 3. `DevCE_GreatWorksArchive`
Keeps great works that would be lost (a relic found with no free slot, a work whose building is removed) in an "Archive" you can view, place and trade, from a button on the Great Works screen. The native part is switched on only by this mod's game capability, so Dev CE alone plays like vanilla. Experimental: some cases are untested (city capture, spy heist, no free slot for the receiver, DX11). Needs Dev CE.

## 4. `experiments_cru121`: demos and experiments
- `DevCE_Playground`: button panels to try the new engine functions.
- `FlattenHillsView`: flattening a hill by script also flattens it on the 3D map (works without Dev CE).
- `DevCE_AudioTest`: plays wav/ogg/mp3 through the new Audio API.
- `DevCE_DebugDemo`: a script to try the Lua debugger on (see `Tools`).

## 5. `experiments_slothoth`: features by [slothoth](https://github.com/slothoth/civ6-dev-ce) (check his repo for updates)
Feature mods (data tables or properties a modder can use), each followed by its test mod:
- `DevCE_BuildingGates` (+`Test`): buildings/wonders a city can build only while a property is set.
- `DevCE_CitySpecialists` (+`Test`): specialists worked in the city centre.
- `DevCE_TechGates` (+`Test`): extra conditions on techs and civics.
- `DevCE_UnitCommandActions` (+`_Test`): unit commands that trigger a great person action.
- `DevCE_UnitPropertyRequirements`: requirement type for unit properties.
- `DevCE_UnitUpgrades`, `DevCE_UpgradeFallback`, `DevCE_UnitAltUpgradeTest`: upgrade rules, fallbacks, alternate upgrade paths.
- `DevCE_GoldPurchase` (+`Test`): buy build-only units and buildings with gold.
- Test-only: `DevCE_CityRadiusTest` (city work radius up to 5), `DevCE_FoodProductionTest`, `DevCE_FounderUniquesTest`, `DevCE_DistrictCitizensTest`.
The test mods change the game on purpose (cheap wonders, free gold, ...): use them in throwaway games only.

## 6. henno
henno's AI district-score API (`CE_AI*` Lua functions, [his repo](https://github.com/hennogous/CivilizationVI_CommunityExtension)) is built into the DLL but inert until a mod publishes a policy. No mod for it ships yet.

## 7. Other files
- `Tools/`: command-line Lua debugger client and Debug Adapter (for editors); see `README_DEBUGGER.md`.
- `CUSTOM_PARKS.md`, `TERRAIN_VIEW.md`, `VETO_EVENTS.md`: documentation of those features.
- `README-INSTALL.txt`: install steps, release history, warnings. `LICENSE.txt`: AGPL-3.0.
