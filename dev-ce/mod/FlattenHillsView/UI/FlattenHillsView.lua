-- Flatten Hills View (UI only). A tile that was HILLS when the game was loaded and is flat land now gets the engine's own heightmap edit LM_HEX_FLATTEN (placed with
-- AssetPreview.SpoofLandmarkAt); when it becomes hills again the edit is removed (ClearLandmarkAt) and the real hill is back. Nothing here changes game data.
-- The map is rebuilt from the game data on every load, so the baseline ("was hills") is taken when this context starts and the spoofed edits are never saved.

local FLATTEN_NAME = "LM_HEX_FLATTEN";

FHV = FHV or {};          -- global on purpose: the Frida test harness runs this file in a persistent state and calls FHV.Sync()
FHV.baseHill = nil;       -- plot index -> true: the plot was hills when the world view was built
FHV.flattened = {};       -- plot index -> true while our edit is placed
FHV.dirty = false;
FHV.index = nil;          -- landmark index of LM_HEX_FLATTEN
FHV.props = nil;
FHV.warned = false;

FHV.lines = FHV.lines or {};
local function Log(msg)
	print("[FlattenHillsView] " .. tostring(msg));
	table.insert(FHV.lines, tostring(msg));
	while #FHV.lines > 12 do table.remove(FHV.lines, 1); end
	if Controls ~= nil and Controls.Fhv_Status ~= nil then pcall(function() Controls.Fhv_Status:SetText(table.concat(FHV.lines, "\n")); end); end
end

local function TerrainName(terrainType)
	local row = GameInfo.Terrains[terrainType];
	return row and row.TerrainType or "";
end

local function IsHills(terrainType) return TerrainName(terrainType):find("_HILLS$") ~= nil; end

-- flat land only: grass, plains, desert, tundra, snow (mountains, coast and ocean change more than a height: not handled)
local function IsFlatLand(terrainType)
	local n = TerrainName(terrainType);
	if n == "" or n:find("_HILLS$") or n:find("_MOUNTAIN$") or n == "TERRAIN_COAST" or n == "TERRAIN_OCEAN" then return false; end
	return true;
end

local function FindFlatten()
	if AssetPreview == nil or AssetPreview.GetLandmarkCount == nil then
		if not FHV.warned then FHV.warned = true; Log("AssetPreview is not available in this UI context: nothing will be flattened."); end
		return false;
	end
	for i = 0, AssetPreview.GetLandmarkCount() - 1 do
		if AssetPreview.GetLandmarkName(i) == FLATTEN_NAME then
			local _, props = next(AssetPreview.GetLandmarkAssetList(i));
			if props ~= nil then FHV.index = i; FHV.props = props; return true; end
		end
	end
	if not FHV.warned then FHV.warned = true; Log(FLATTEN_NAME .. " not found among the landmarks."); end
	return false;
end

-- Yield icons (the 'Show Yield Icons' map option): PlotInfo.lua fills the 'Yield_Icons' lens layer once when the game loads and the game never updates it when a script
-- changes the terrain. This is PlotInfo.lua's own logic (GetPlotYields / UpdateYieldIcons, copied from Base/Assets/UI/WorldView/PlotInfo.lua) for the given plots only.
local YIELD_NUMBER_VARIATION = "Yield_Variation_";
local YIELD_VARIATION_MANY = "Yield_Variation_Many";
local YIELD_VARIATION_MAP = {
	YIELD_FOOD = "Yield_Food_", YIELD_PRODUCTION = "Yield_Production_", YIELD_GOLD = "Yield_Gold_",
	YIELD_SCIENCE = "Yield_Science_", YIELD_CULTURE = "Yield_Culture_", YIELD_FAITH = "Yield_Faith_",
};

local function RefreshYields(plotIds)
	if UILens == nil or #plotIds == 0 then return; end
	local cityCenter = GameInfo.Districts["DISTRICT_CITY_CENTER"].Index;
	local layer = UILens.CreateLensLayerHash("Yield_Icons");
	local yields = {};
	for _, plotId in ipairs(plotIds) do
		local plot = Map.GetPlotByIndex(plotId);
		local districtType = plot:GetDistrictType();
		if districtType == -1 or districtType == cityCenter then
			for row in GameInfo.Yields() do
				local amount = plot:GetYield(row.Index);
				if amount > 0 and YIELD_VARIATION_MAP[row.YieldType] ~= nil then
					local key = YIELD_VARIATION_MAP[row.YieldType] .. (amount > 5 and 5 or amount);
					local entry = yields[key];
					if entry == nil then entry = { data = {}, variations = {}, yieldType = row.YieldType }; yields[key] = entry; end
					table.insert(entry.data, plotId);
					if amount > 5 then
						if amount > 11 then table.insert(entry.variations, { YIELD_VARIATION_MANY, plotId });
						else table.insert(entry.variations, { YIELD_NUMBER_VARIATION .. amount, plotId }); end
					end
				end
			end
		end
	end
	UILens.ClearHexes(layer, plotIds);          -- SetLayerHexesArea only ADDS: remove the old icons of these plots first (what PlotInfo.lua's OnMapYieldsChanged does)
	-- in the order of GameInfo.Yields() like PlotInfo.lua's UpdateYieldIcons: the icons of a hex are placed in the order they are added (pairs() order is arbitrary and swapped food and production)
	for row in GameInfo.Yields() do
		for key, entry in pairs(yields) do
			if entry.yieldType == row.YieldType then
				if GameConfiguration.IsWorldBuilderEditor() then UILens.SetLayerHexesArea(layer, 0, entry.data, entry.variations, key);
				else UILens.SetLayerHexesArea(layer, Game.GetLocalPlayer(), entry.data, entry.variations, key); end
			end
		end
	end
end

function FHV.Snapshot()
	FHV.baseHill = {};
	FHV.flattened = {};
	FHV.last = {};            -- plot index -> terrain type at the last sync (to find the plots whose terrain changed)
	local n = 0;
	for idx = 0, Map.GetPlotCount() - 1 do
		local plot = Map.GetPlotByIndex(idx);
		if plot ~= nil then
			local t = plot:GetTerrainType();
			FHV.last[idx] = t;
			if IsHills(t) then FHV.baseHill[idx] = true; n = n + 1; end
		end
	end
	Log("baseline: " .. n .. " hills plots");
end

function FHV.Sync()
	FHV.dirty = false;
	if FHV.baseHill == nil then return 0; end
	local changedPlots = {};
	for idx = 0, Map.GetPlotCount() - 1 do
		local t = Map.GetPlotByIndex(idx):GetTerrainType();
		if FHV.last[idx] ~= t then FHV.last[idx] = t; table.insert(changedPlots, idx); end
	end
	if #changedPlots > 0 then
		local ok, err = pcall(RefreshYields, changedPlots);
		if not ok then Log("yield icon refresh failed: " .. tostring(err)); end
	end
	if FHV.index == nil and not FindFlatten() then return 0; end
	local changed = 0;
	for idx, _ in pairs(FHV.baseHill) do
		local plot = Map.GetPlotByIndex(idx);
		local t = plot:GetTerrainType();
		local x, y = plot:GetX(), plot:GetY();
		if IsFlatLand(t) then
			if not FHV.flattened[idx] then
				local p = FHV.props;
				AssetPreview.SpoofLandmarkAt(x, y, p.civ, p.era, p.appeal, 0, "Worked", FHV.index, p.variant or 0);
				FHV.flattened[idx] = true; changed = changed + 1;
				Log("flattened (" .. x .. "," .. y .. ") " .. TerrainName(t));
			end
		elseif IsHills(t) and FHV.flattened[idx] then
			AssetPreview.ClearLandmarkAt(x, y);
			FHV.flattened[idx] = nil; changed = changed + 1;
			Log("hill restored (" .. x .. "," .. y .. ")");
		end
	end
	return changed;
end

local function OnTerrainTypeChanged()
	FHV.dirty = true;
	local ok, err = pcall(FHV.Sync);
	if not ok then Log("Sync failed: " .. tostring(err)); end
end

local function OnUpdate()
	if FHV.dirty then
		local ok, err = pcall(FHV.Sync);
		if not ok then FHV.dirty = false; Log("Sync failed: " .. tostring(err)); end
	end
end

local function OnLoadGameViewStateDone()
	-- the world view is (re)built from the game data now: new baseline, no edits placed
	local ok, err = pcall(FHV.Snapshot);
	if not ok then Log("Snapshot failed: " .. tostring(err)); end
end

local function Initialize()
	Log("context started");
	local ok, err = pcall(FHV.Snapshot);
	if not ok then Log("Snapshot failed (retried at load end): " .. tostring(err)); end
	-- every registration on its own: one failure must not leave the others unregistered
	local steps = {
		{ "TerrainTypeChanged", function() Events.TerrainTypeChanged.Add(OnTerrainTypeChanged); end },
		{ "LoadGameViewStateDone", function() Events.LoadGameViewStateDone.Add(OnLoadGameViewStateDone); end },
		{ "SetUpdate", function() ContextPtr:SetUpdate(OnUpdate); end },
	};
	for _, st in ipairs(steps) do
		local ok2, err2 = pcall(st[2]);
		Log("register " .. st[1] .. ": " .. (ok2 and "ok" or tostring(err2)));
	end
end

-- the Frida test harness runs this file without a ContextPtr: only define the functions then
if ContextPtr ~= nil then Initialize(); end
