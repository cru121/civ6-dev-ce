-- =====================================================================
--  DevCE_Terrain_Gameplay.lua   (runs in the GAMEPLAY Lua context)
--
--  Terrain experiments for the Playground panel (left "Terrain" panel). Same transport as the other buttons:
--  the UI sends an EXECUTE_SCRIPT unit command named like the GameEvents entry; the handler gets (owner, unit id, parameters).
--  The unit that carries the command is the selected unit (else the first unit); the terrain buttons act on ITS tile.
--
--  Background (see dev-ce/TERRAIN_VIEW.md): TerrainBuilder.SetTerrainType changes the gameplay data and the
--  UI cache (tooltip, yields). Plain Dev CE: the 3D map is only rebuilt when the game starts or loads (features like forests DO refresh
--  live: the Forest buttons are the control experiment). With the Dev CE TEST BUILD (terrain view patch, marker file TerrainView.patch) the
--  map follows flat <-> hills changes at once (dome, textures, trees, resources, improvements); Coast, Mountain and Ocean still need a
--  save + load. Self-contained on purpose (gameplay scripts do not reliably share globals).
-- =====================================================================

print("[DevCEPg][TERR] loading...");

local seq = 0;
local function Report(owner, msg)
	seq = seq + 1000;   -- far away from the main script's counter, so the UI always sees a change
	seq = seq + math.random(1, 999);
	print("[DevCEPg][TERR] " .. msg);
	pcall(function()
		Players[owner]:SetProperty("DEVCEPG_MSG", msg);
		Players[owner]:SetProperty("DEVCEPG_SEQ", seq .. "t");
	end);
end

-- the Undo button: every button press is one group of {plot index, terrain type before}
local history = {};
local function BeginGroup()
	history[#history + 1] = {};
	while #history > 30 do table.remove(history, 1); end
end
local function RecordBefore(plot)
	local g = history[#history];
	if g ~= nil then g[#g + 1] = { plot:GetIndex(), plot:GetTerrainType() }; end
end

local function GetUnit(owner, unitID)
	local u = nil;
	pcall(function() u = Players[owner]:GetUnits():FindID(unitID); end);
	return u;
end

local function UnitPlot(owner, unitID)
	local u = GetUnit(owner, unitID);
	if u == nil then return nil, nil, "no carrier unit (select a unit first)"; end
	local plot = Map.GetPlot(u:GetX(), u:GetY());
	if plot == nil then return nil, u, "no plot under the unit"; end
	return plot, u, nil;
end

local function TerrainName(plot)
	local t = plot:GetTerrainType();
	local row = GameInfo.Terrains[t];
	return (row and row.TerrainType or "?") .. " (" .. tostring(t) .. ")";
end

local function FeatureName(plot)
	local f = plot:GetFeatureType();
	if f == nil or f < 0 then return "none"; end
	local row = GameInfo.Features[f];
	return row and row.FeatureType or tostring(f);
end

-- The engine's terrain setter does NOT re-validate the feature: it only drops an invalid resource (verified in the DLL), so e.g. floodplains stay
-- on a tile that became hills, and a grassland floodplain stays "grassland" after the tile became plains. Normalise after every change:
-- take the feature off, ask the engine whether it is still allowed, otherwise use the matching floodplains variant, otherwise leave it removed.
local function FloodplainVariant(terrainType)
	if terrainType:find("_HILLS$") or terrainType:find("_MOUNTAIN$") or terrainType == "TERRAIN_COAST" or terrainType == "TERRAIN_OCEAN" then return nil; end
	if terrainType:find("GRASS") then return "FEATURE_FLOODPLAINS_GRASSLAND"; end
	if terrainType:find("PLAINS") then return "FEATURE_FLOODPLAINS_PLAINS"; end
	if terrainType:find("DESERT") then return "FEATURE_FLOODPLAINS"; end
	return nil;
end
local function NormalizeFeature(plot)
	local f = plot:GetFeatureType();
	if f == nil or f < 0 then return ""; end
	local frow = GameInfo.Features[f];
	local trow = GameInfo.Terrains[plot:GetTerrainType()];
	if frow == nil or trow == nil then return ""; end
	local note = "";
	pcall(function()
		TerrainBuilder.SetFeatureType(plot, -1);
		if TerrainBuilder.CanHaveFeature(plot, f) then
			TerrainBuilder.SetFeatureType(plot, f);          -- still valid, put it back
			return;
		end
		local alt = nil;
		if frow.FeatureType:find("^FEATURE_FLOODPLAINS") then
			local v = FloodplainVariant(trow.TerrainType);
			alt = v and GameInfo.Features[v] or nil;
		end
		if alt ~= nil and TerrainBuilder.CanHaveFeature(plot, alt.Index) then
			TerrainBuilder.SetFeatureType(plot, alt.Index);
			note = " Feature " .. frow.FeatureType:gsub("FEATURE_", "") .. " became " .. alt.FeatureType:gsub("FEATURE_", "") .. ".";
		else
			note = " Feature " .. frow.FeatureType:gsub("FEATURE_", "") .. " removed (not valid on the new terrain).";
		end
	end);
	return note;
end

local function Set(plot, typeName)
	local row = GameInfo.Terrains[typeName];
	if row == nil then return false, "unknown terrain " .. typeName, ""; end
	if TerrainBuilder == nil or TerrainBuilder.SetTerrainType == nil then return false, "TerrainBuilder.SetTerrainType is not available in this VM", ""; end
	if plot:GetTerrainType() ~= row.Index then RecordBefore(plot); end
	local ok, err = pcall(function() TerrainBuilder.SetTerrainType(plot, row.Index); end);
	local note = "";
	if ok then note = NormalizeFeature(plot); end     -- also runs when the terrain was already the target: it repairs tiles left inconsistent earlier
	return ok, err, note;
end

-- ---------------------------------------------------------------- set the terrain under the unit
local function SetUnderUnit(owner, unitID, typeName)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Terrain: " .. why); return; end
	BeginGroup();
	local before = TerrainName(plot);
	local ok, err, note = Set(plot, typeName);
	Report(owner, "Terrain " .. typeName:gsub("TERRAIN_", "") .. " at " .. plot:GetX() .. "," .. plot:GetY() .. ": " .. (ok and "ok" or ("failed: " .. tostring(err)))
		.. "; " .. before .. " -> " .. TerrainName(plot) .. "." .. (note or "") .. " Tooltip and yields follow at once; the 3D map follows with the Dev CE test build (flat <-> hills), Coast/Mountain/Ocean need a save + load.");
end

local SIMPLE = {
	{ "DevCEPg_T_Grass",       "TERRAIN_GRASS" },
	{ "DevCEPg_T_Plains",      "TERRAIN_PLAINS" },
	{ "DevCEPg_T_Desert",      "TERRAIN_DESERT" },
	{ "DevCEPg_T_Tundra",      "TERRAIN_TUNDRA" },
	{ "DevCEPg_T_Snow",        "TERRAIN_SNOW" },
	{ "DevCEPg_T_Coast",       "TERRAIN_COAST" },
	{ "DevCEPg_T_GrassHills",  "TERRAIN_GRASS_HILLS" },
	{ "DevCEPg_T_PlainsHills", "TERRAIN_PLAINS_HILLS" },
	{ "DevCEPg_T_DesertHills", "TERRAIN_DESERT_HILLS" },
	{ "DevCEPg_T_TundraHills", "TERRAIN_TUNDRA_HILLS" },
	{ "DevCEPg_T_SnowHills",   "TERRAIN_SNOW_HILLS" },
	{ "DevCEPg_T_GrassMtn",    "TERRAIN_GRASS_MOUNTAIN" },
	{ "DevCEPg_T_Ocean",       "TERRAIN_OCEAN" },
};
for _, s in ipairs(SIMPLE) do
	local name = s[2];
	GameEvents[s[1]].Add(function(owner, unitID) SetUnderUnit(owner, unitID, name); end);
end

-- ---------------------------------------------------------------- cycle through the land terrains
local CYCLE = { "TERRAIN_GRASS", "TERRAIN_GRASS_HILLS", "TERRAIN_PLAINS", "TERRAIN_PLAINS_HILLS", "TERRAIN_DESERT", "TERRAIN_DESERT_HILLS",
	"TERRAIN_TUNDRA", "TERRAIN_TUNDRA_HILLS", "TERRAIN_SNOW", "TERRAIN_SNOW_HILLS" };
GameEvents.DevCEPg_T_Cycle.Add(function(owner, unitID)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Terrain cycle: " .. why); return; end
	local cur = GameInfo.Terrains[plot:GetTerrainType()];
	local nextIdx = 1;
	for i, n in ipairs(CYCLE) do if cur ~= nil and cur.TerrainType == n then nextIdx = i % #CYCLE + 1; end end
	SetUnderUnit(owner, unitID, CYCLE[nextIdx]);
end);

-- ---------------------------------------------------------------- experiment: hills <-> flat of the same base terrain
GameEvents.DevCEPg_T_HillSwap.Add(function(owner, unitID)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Hills swap: " .. why); return; end
	local cur = GameInfo.Terrains[plot:GetTerrainType()];
	if cur == nil then Report(owner, "Hills swap: unknown terrain"); return; end
	local t = cur.TerrainType;
	local target = nil;
	if t:find("_HILLS$") then target = t:gsub("_HILLS$", "");
	elseif t:find("_MOUNTAIN$") then target = t:gsub("_MOUNTAIN$", "");
	elseif t == "TERRAIN_COAST" or t == "TERRAIN_OCEAN" or t == "TERRAIN_LAKE" then Report(owner, "Hills swap: water has no hills version."); return;
	else target = t .. "_HILLS"; end
	SetUnderUnit(owner, unitID, target);
end);

-- ---------------------------------------------------------------- experiment: the six neighbours become plains (land tiles without a city only)
GameEvents.DevCEPg_T_RingPlains.Add(function(owner, unitID)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Ring: " .. why); return; end
	BeginGroup();
	local changed, skipped = 0, 0;
	local x, y = plot:GetX(), plot:GetY();
	for dir = 0, 5 do
		local n = Map.GetAdjacentPlot(x, y, dir);
		if n ~= nil and not n:IsWater() and not n:IsCity() and not n:IsNaturalWonder() then
			local ok = Set(n, "TERRAIN_PLAINS");
			if ok then changed = changed + 1; else skipped = skipped + 1; end
		else
			skipped = skipped + 1;
		end
	end
	Report(owner, "Ring around " .. x .. "," .. y .. ": " .. changed .. " tiles set to plains, " .. skipped .. " skipped (water, city, wonder). With the Dev CE test build they redraw at once.");
end);

-- ---------------------------------------------------------------- hills in a ring / field around the unit, flat again, undo
local function IsFlatLand(row)
	local n = row.TerrainType;
	return not (n:find("_HILLS$") or n:find("_MOUNTAIN$") or n == "TERRAIN_COAST" or n == "TERRAIN_OCEAN" or n == "TERRAIN_LAKE");
end
local function IsHillsLand(row) return row.TerrainType:find("_HILLS$") ~= nil; end

-- radius 1 = the six neighbours, radius 2 = the 18 tiles around; centre = also the unit's own tile
local function AroundUnit(owner, unitID, radius, withCentre, wantHills, label)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, label .. ": " .. why); return; end
	BeginGroup();
	local x, y = plot:GetX(), plot:GetY();
	local changed, skipped = 0, 0;
	for dx = -radius - 1, radius + 1 do for dy = -radius - 1, radius + 1 do
		local n = Map.GetPlot(x + dx, y + dy);
		if n ~= nil then
			local d = Map.GetPlotDistance(x, y, n:GetX(), n:GetY());
			if d <= radius and (d > 0 or withCentre) then
				local row = GameInfo.Terrains[n:GetTerrainType()];
				local target = nil;
				if row ~= nil and not n:IsWater() and not n:IsCity() and not n:IsNaturalWonder() then
					if wantHills and IsFlatLand(row) then target = row.TerrainType .. "_HILLS";
					elseif (not wantHills) and IsHillsLand(row) then target = row.TerrainType:gsub("_HILLS$", ""); end
				end
				if target ~= nil and GameInfo.Terrains[target] ~= nil then
					local ok = Set(n, target);
					if ok then changed = changed + 1; else skipped = skipped + 1; end
				end
			end
		end
	end end
	Report(owner, label .. " around " .. x .. "," .. y .. ": " .. changed .. " tiles changed" .. (skipped > 0 and (", " .. skipped .. " failed") or "") .. ". Water, cities, wonders, mountains and tiles that already match are left alone. The Undo button restores them.");
end
GameEvents.DevCEPg_T_RingHills.Add(function(owner, unitID) AroundUnit(owner, unitID, 1, false, true, "Ring of hills"); end);
GameEvents.DevCEPg_T_FieldHills.Add(function(owner, unitID) AroundUnit(owner, unitID, 2, true, true, "Field of hills (radius 2)"); end);
GameEvents.DevCEPg_T_RingFlat.Add(function(owner, unitID) AroundUnit(owner, unitID, 2, true, false, "Flatten the hills (radius 2)"); end);

GameEvents.DevCEPg_T_Undo.Add(function(owner, unitID)
	local g = table.remove(history);
	if g == nil then Report(owner, "Undo: nothing to undo."); return; end
	local n = 0;
	for i = #g, 1, -1 do
		local plot = Map.GetPlotByIndex(g[i][1]);
		if plot ~= nil then
			local ok = pcall(function() TerrainBuilder.SetTerrainType(plot, g[i][2]); end);
			if ok then NormalizeFeature(plot); n = n + 1; end
		end
	end
	Report(owner, "Undo: " .. n .. " tile(s) restored to their previous terrain.");
end);

-- ---------------------------------------------------------------- control experiment: features DO refresh live
local function Forest(owner, unitID, on)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Forest: " .. why); return; end
	local row = GameInfo.Features["FEATURE_FOREST"];
	if row == nil then Report(owner, "Forest: FEATURE_FOREST not found."); return; end
	local before = FeatureName(plot);
	local ok, err = pcall(function()
		if on then
			if TerrainBuilder.CanHaveFeature ~= nil and not TerrainBuilder.CanHaveFeature(plot, row.Index) then error("a forest cannot grow on " .. TerrainName(plot)); end
			TerrainBuilder.SetFeatureType(plot, row.Index);
		else
			TerrainBuilder.SetFeatureType(plot, -1);
		end
	end);
	Report(owner, "Forest " .. (on and "ON" or "OFF") .. " at " .. plot:GetX() .. "," .. plot:GetY() .. ": " .. (ok and "ok" or ("failed: " .. tostring(err)))
		.. "; feature " .. before .. " -> " .. FeatureName(plot) .. ". Features should redraw at once (that is the control for the terrain buttons).");
end
GameEvents.DevCEPg_T_ForestOn.Add(function(owner, unitID) Forest(owner, unitID, true); end);
GameEvents.DevCEPg_T_ForestOff.Add(function(owner, unitID) Forest(owner, unitID, false); end);

-- ---------------------------------------------------------------- readout of the tile as the gameplay side sees it
GameEvents.DevCEPg_T_Readout.Add(function(owner, unitID)
	local plot, u, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Readout: " .. why); return; end
	local parts = { "tile " .. plot:GetX() .. "," .. plot:GetY(), "terrain " .. TerrainName(plot), "feature " .. FeatureName(plot) };
	pcall(function() parts[#parts + 1] = "hills " .. tostring(plot:IsHills()) .. " mountain " .. tostring(plot:IsMountain()) .. " water " .. tostring(plot:IsWater()); end);
	pcall(function()
		local y = {};
		for _, n in ipairs({ "FOOD", "PRODUCTION", "GOLD", "SCIENCE", "CULTURE", "FAITH" }) do y[#y + 1] = n:sub(1, 1) .. plot:GetYield(YieldTypes[n]); end
		parts[#parts + 1] = "yields " .. table.concat(y, " ");
	end);
	pcall(function() parts[#parts + 1] = "appeal " .. tostring(plot:GetAppeal()); end);
	pcall(function() parts[#parts + 1] = "owner " .. tostring(plot:GetOwner()); end);
	Report(owner, "Gameplay readout: " .. table.concat(parts, ", "));
end);

print("[DevCEPg][TERR] ready.");
