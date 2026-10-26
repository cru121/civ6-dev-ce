# Terrain view: the 3D map follows `TerrainBuilder.SetTerrainType` (experimental)

In the retail game a script can change the terrain of a tile (`TerrainBuilder.SetTerrainType(plot, terrain)`), and the game data, yields, tooltips and rules follow at once, but the **3D map keeps drawing the old terrain until the game is loaded again**. The terrain of the map lives in immutable GPU buffers that are built once when the world is created.

With the terrain view enabled, Dev CE patches those buffers when a script changes a tile, so the map shows the change within about a second, without save and load:

| change | 3D map |
|---|---|
| flat land to hills (grass, plains, desert, tundra, snow) | a hill grows (a smooth dome of 1300 height units on top of the ground, units and trees stand on it) and the ground texture changes to the hills texture |
| hills to flat land | with the mod `FlattenHillsView` (UI-only, included) the hill is flattened with the engine's own heightmap edit |
| flat to flat, hills to hills of another kind | the ground texture changes |
| features, resources, improvements on a changed tile | removed and placed again so they stand on the new ground (also the pillaged state; natural wonders are skipped) |
| **mountains, coast, ocean, lakes** | **not supported**: they change the shape of the map in ways this patch does not model. The game data changes as always, the map needs a save and load |

It is a **view only**: nothing is written into saves. A loaded game builds its terrain from the game data, which is correct. Changing a tile back to its original terrain restores the original buffers exactly.

## Switching it on and off
The release zip has the feature **switched on**. The switch is a pair of **empty marker files next to the DLL** (`Documents/My Games/Sid Meier's Civilization VI/Mods/DevCE/Binaries/Win64/`): when the DLL starts it looks for them, and without them it does not touch the terrain at all (no hook is installed).

| file | effect |
|---|---|
| `TerrainView.patch` | hooks the terrain setter and patches the map (this is the feature; in the zip) |
| `TerrainView.objects` | also re-places features, resources and improvements of changed tiles (needs `TerrainView.patch`; in the zip) |
| `TerrainView.selftest` | optional, not in the zip: at world start reads all cells back from the GPU, compares them with the kept copies and checks them (read-only, result in `DevBridge.log`) |
| `TerrainView.noshadow` | optional, not in the zip: do not keep copies of the terrain buffers (DX11 only: the original data is then read back from the GPU, which needs a hook on the D3D11 draw calls) |
| `TerrainView.compare` | optional, not in the zip, diagnostics: logs every buffer name the game creates and, on DX11, compares each read from the kept copies with a real GPU read (`COMPARE:` lines in `DevBridge.log` if they ever differ) |
| `TerrainView.patchtest` | optional, not in the zip: at world start raises and reverts a plot and checks the buffers (**changes the map briefly**, result in `DevBridge.log`) |

**Delete `TerrainView.patch` to switch the whole feature off** (also when you want to find out whether a problem comes from the terrain view); delete `TerrainView.objects` to keep the map patch but not the re-placing of trees, resources and improvements. The change applies at the next start of the game. If you build the DLL yourself, `dev-ce/mod/DevCE/Binaries/Win64/` in the repository already holds the two files.

To try it: enable **Dev CE (experimental)**, **Dev CE Playground** and **Flatten Hills View**, start a single-player game, select a unit and use the *Terrain* and *Tile objects* panels on the left (see the Playground section of the README). Flatten Hills View also refreshes the yield icons of changed tiles (the game fills them once at load).

## How it works (for modders)
* Terrain is drawn from render cells of 256 x 256 texels (22 x 11 cells on a Small map, 53 x 26 on a Huge one); a cell record (944 bytes) names the GPU buffers of its **ID mesh** (materials) and **base mesh** (heights). The buffers are immutable, so a patch builds new buffers from a copy of the original data and swaps the handles in the record, then sets the cell's dirty bit.
* **The original data** comes from copies kept when the game creates the terrain buffers (a hook on the game's `CreateVB` / `CreateIB` keeps the bytes of every `Terrain_Cell*` buffer, about 17 MiB on a Small map). No GPU access is needed, so nothing is hooked in the graphics runtime, and it is the only source on DX12.
* **The second base mesh section** of edited cells (coastal cells in practice) lives in `Terrain_CellEdit_VB` / `_IB`, which are created empty and filled at run time. The game keeps a CPU mirror of each (`WorldView_TerrainContext_HeightEdit.cpp`, a 512 KB block per buffer). The mirror is written the moment a terrain change makes the game bake a cell again, while the GPU copy follows later, so reading it live at that moment returns the new data instead of the generated world (a tile with a wildly over-subdivided, corrupted mesh). The patch therefore takes a snapshot of the mirrors (and of every cell's second-section record fields) before the first terrain change of a world and reads each cell's second section once: from the snapshot while the record still points at the range the world was generated with, and from the live mirror once the game has baked the cell again (a neighbouring change does that, and the new section sits at a range the snapshot never saw). On DX11 the mirrors were checked byte for byte against the GPU.
* **GPU fallback (DX11 only).** If a buffer has no copy, the data is read back from the GPU (one staging buffer; a whole Huge map in about 50 ms) by a hook on the context's draw calls; if the usual inline hook fails (overlays that patch the same functions), the two vtable entries are replaced instead. The start-up self-test compares the kept copies with a full GPU read. DX12 has no per-handle buffer object to read back, so a missing copy means the change is not applied (and logged).
* `Terrain::Builder::_SetTerrainType` (both public overloads and the Lua call end there) is hooked; a worker thread batches changes and recomputes the affected cells **from the original data and the net change of every plot**, so changes are order independent and a plot put back restores the original exactly.
* A dome needs a finer mesh than the game generates (flat ground is a few huge triangles): inside the dome zone every edge longer than 8 texels is split at its midpoint (watertight), except edges that touch the ocean (splitting those draws a dark trench along the shore).
* Details that cost days to find, valid for the installed build: ocean heights are negative int16 values stored as u16 (average them as signed); a cell can carry a **second base mesh section** (record +0x174..+0x188) that the game draws instead of the first one and rewrites when it bakes the cell, so it is patched and checked again every 150 ms; the base mesh is split in **four quadrant chunks** with bounding boxes and (start, count) ranges in the record (+0x44 + 0x20 k, +0x5c.., +0x60..), which must be rewritten when the index buffer changes and whose boxes must be raised; neighbouring cell meshes overlap.
* Units and trees follow the ground through the engine's ground height sampler (`Terrain::Sampler::GetWorldHeight`), which is detoured to add the dome.
* Features, resources and improvements are re-placed with the game's own builders on the game thread (a hook on `Game::Autoplay::Manager::Update`, the per-tick function).

## Limits
* **Windows, the DX11 exe (`CivilizationVI.exe`) or the DX12 exe (`CivilizationVI_DX12.exe`), the same two Steam builds as the rest of Dev CE (15038592, 15296837).** The log says which one it found (`build check: OK ... (DX12 exe)`). If a Steam launch option such as `"...\CivilizationVI.exe" %command%` is set, Steam always starts the DX11 exe, whatever you pick in its launch dialog. Fixed addresses (a table per exe); the exe's PE timestamp and size are checked at startup and the feature disables itself, with a line in `DevBridge.log`, on any other build.
* The terrain-to-material table assumes the vanilla order of the terrain types. Mods that add terrain types are not handled.
* A tile is patched relative to the terrain it had **when it was first seen** (the generated mesh). A tile that was hills when the world was built and is flattened by a script needs `FlattenHillsView`.
* Dome constants are compiled in (height 1300, full out to 12 texels, zero at 76).
* **Single player only.** Not tested in multiplayer. Not tested with other mods that replace terrain art or shaders.
* Pillaged improvements: the code path exists, no test map had one.
* Tile objects (features, resources, improvements) were tested on DX11 only; on DX12 they use the same GameCore code, but no one has run them there yet.
* A hairline seam, a few pixels wide, was seen once on DX12 along a coast at an extreme zoom and a sharp camera angle; not investigated (it is not known whether it also exists on DX11).

## What was tested
Steam build 15038592, single player: reader byte-identical to a full GPU read on Small and Huge maps; material patch and revert; domes on flat grass/plains/desert tiles including coastal tiles, river tiles and a peninsula; clusters of 4 plots and about 35 random plots on a Small map and 24 plots plus a ring around the player's settler on a Huge map, with no seams, cracks or holes (checked by looking at the map from all angles and by scanning the live meshes for uncovered points); revert by changing the data back (every record field returns to the original); save and load; Better Restart into a Huge game; a startup self-test (42 checks). **Not tested:** other builds, multiplayer, mountains/coast/ocean (unsupported), pillaged improvements, desert/tundra/snow domes beyond a few tiles.

**0.7.0, DX12 exe, build 15038592, single player (a Standard map):** start-up checks pass (every kept copy has the size of the exe's own record for its handle; the patch, revert and dome self-tests pass); a coastal flat tile to plains hills and back (the second base mesh section is patched and restored); a ring and a field of hills around a unit, including coastal tiles; flat to desert, snow, snow hills and tundra, plains hills to tundra hills; save and load (the patch state is dropped and the new world view is picked up again); no failed read, no crash. **DX11 after the 0.7.0 changes:** see the release notes. Not tested on DX12: tile objects, a Huge map, Better Restart.

## Diagnostics
The DLL exports `TerrainView_Command(const char*)` (used by the Frida tools of this project): `status`, `undo`, `patch X Y OLD NEW`, `replace X Y`, `cellinfo X Y`, `height X Y`, `hprofile X Y`, `holes X Y`, `freeze X Y`, `unfreeze`, `overlay on|off`, `objects on|off`, `ampscale V`, `seamfix on|off`, `splitshore on|off`, `shorefade W`. All log lines start with `[TerrainView]` in `DevBridge.log`.

