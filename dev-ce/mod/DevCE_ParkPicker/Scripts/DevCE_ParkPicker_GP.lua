-- =====================================================================
--  DevCE_ParkPicker_GP.lua   (gameplay Lua context)
--
--  Handles the UI command DEVCEPARK_DESIGNATE (EXECUTE_SCRIPT unit command: PARAM_X = plot1 * 4096 + plot2, PARAM_Y = plot3 * 4096 + plot4)
--  from UI/DevCE_ParkPicker.lua. Re-checks everything, then calls the Dev CE native NationalParks.DesignateCustom and does what the
--  vanilla DesignatePark operation does for the unit: one park charge less, and the Naturalist is used up when it has none left
--  (otherwise its moves end).
--  Answer for the UI: player properties DEVCEPARK_MSG, then DEVCEPARK_SEQ (incremented last).
-- =====================================================================

local BASE = 4096;
local MAX_RADIUS = 4;        -- keep equal to the UI script
local TILE_COUNT = 4;


-- ---- District policy (KEEP IDENTICAL in UI/DevCE_ParkPicker.lua and Scripts/DevCE_ParkPicker_GP.lua) --------------------------------
-- The game's own park rule rejects every tile with a district. These switches let some districts through; the native
-- NationalParks.IsValidParkTileEx(player, plot, 3) then applies the remaining rules (owner, no improvement, not in a park) and ignores
-- the terrain/appeal requirement for the tiles listed here.
local ALLOW_WORLD_WONDERS  = true;
local WONDER_WHITELIST     = nil;   -- nil = every world wonder; or e.g. { BUILDING_STONEHENGE = true, BUILDING_HANGING_GARDENS = true }
local ALLOW_PRESERVE       = true;

local function ParkTileValid(pid, plotIndex)
	if NationalParks.IsValidParkTile(pid, plotIndex) then return true; end              -- the vanilla rule
	if NationalParks.IsValidParkTileEx == nil then return false; end
	local plot = Map.GetPlotByIndex(plotIndex);
	if plot == nil then return false; end
	local districtIndex = plot:GetDistrictType();
	if districtIndex == nil or districtIndex < 0 then return false; end
	local district = GameInfo.Districts[districtIndex];
	if district == nil then return false; end
	if district.DistrictType == "DISTRICT_WONDER" then
		if not ALLOW_WORLD_WONDERS then return false; end
		local wonderIndex = plot:GetWonderType();
		if wonderIndex == nil or wonderIndex < 0 then return false; end
		if WONDER_WHITELIST ~= nil then
			local building = GameInfo.Buildings[wonderIndex];
			if building == nil or not WONDER_WHITELIST[building.BuildingType] then return false; end
		end
		return NationalParks.IsValidParkTileEx(pid, plotIndex, 3);
	elseif district.DistrictType == "DISTRICT_PRESERVE" and ALLOW_PRESERVE then
		return NationalParks.IsValidParkTileEx(pid, plotIndex, 3);
	end
	return false;
end

local function Reply(owner, text)
	print("[DevCEPark][GP] " .. text);
	local player = Players[owner];
	if player == nil then return; end
	local seq = tonumber(player:GetProperty("DEVCEPARK_SEQ")) or 0;
	player:SetProperty("DEVCEPARK_MSG", text);
	player:SetProperty("DEVCEPARK_SEQ", seq + 1);
end

local function CityKey(plotIndex)
	local key = nil;
	pcall(function()
		local city = Cities.GetPlotPurchaseCity(plotIndex);
		if city ~= nil then key = city:GetOwner() .. ":" .. city:GetID(); end
	end);
	return key;
end

local function Adjacent(a, b)
	local ax, ay = Map.GetPlotLocation(a);
	local bx, by = Map.GetPlotLocation(b);
	return Map.GetPlotDistance(ax, ay, bx, by) == 1;
end

local function Connected(plots)
	local reached, count, changed = { [1] = true }, 1, true;
	while changed do
		changed = false;
		for i = 1, #plots do
			if not reached[i] then
				for j = 1, #plots do
					if reached[j] and Adjacent(plots[i], plots[j]) then reached[i] = true; count = count + 1; changed = true; break; end
				end
			end
		end
	end
	return count == #plots;
end

local CODE_TEXT = {
	[1] = "bad number of tiles", [2] = "a tile is outside the map", [3] = "a tile was picked twice", [4] = "a tile is already part of a park",
	[5] = "the game could not create the park here (map edge)", [6] = "internal error", [7] = "none of the tiles is inside a city's borders",
};

GameEvents.DEVCEPARK_DESIGNATE.Add(function(owner, unitID, parameters)
	local px = parameters and parameters[UnitCommandTypes.PARAM_X];
	local py = parameters and parameters[UnitCommandTypes.PARAM_Y];
	if px == nil or py == nil then Reply(owner, "National Park request without tiles."); return; end
	if NationalParks == nil or NationalParks.DesignateCustom == nil then Reply(owner, "Dev CE is not loaded: custom parks are not available."); return; end

	local plots = { math.floor(px / BASE), px % BASE, math.floor(py / BASE), py % BASE };
	local unit = UnitManager.GetUnit(owner, unitID);
	if unit == nil then Reply(owner, "The Naturalist was not found."); return; end
	local info = GameInfo.Units[unit:GetType()];
	if info == nil or (info.ParkCharges or 0) <= 0 or unit:GetParkCharges() <= 0 then Reply(owner, "This unit cannot designate a National Park."); return; end
	if unit:GetMovementMovesRemaining() <= 0 then Reply(owner, "The Naturalist has no moves left."); return; end
	if #plots ~= TILE_COUNT then Reply(owner, "Wrong number of tiles."); return; end

	local city = CityKey(plots[1]);
	for i = 1, #plots do
		for j = 1, i - 1 do if plots[i] == plots[j] then Reply(owner, "A tile was picked twice."); return; end end
		if Map.GetPlotByIndex(plots[i]) == nil then Reply(owner, "A tile is outside the map."); return; end
		local x, y = Map.GetPlotLocation(plots[i]);
		if Map.GetPlotDistance(x, y, unit:GetX(), unit:GetY()) > MAX_RADIUS then Reply(owner, "A tile is too far from the Naturalist."); return; end
		local okv, valid = pcall(ParkTileValid, owner, plots[i]);
		if not (okv and valid) then Reply(owner, "A tile is not valid for a National Park."); return; end
		local c = CityKey(plots[i]);
		if city ~= nil and c ~= nil and c ~= city then Reply(owner, "All tiles must belong to the same city."); return; end
	end
	if not Connected(plots) then Reply(owner, "The tiles must touch each other."); return; end

	local code = NationalParks.DesignateCustom(owner, plots);
	if code ~= 0 then Reply(owner, "National Park not created: " .. (CODE_TEXT[code] or ("code " .. tostring(code)))); return; end

	pcall(function() unit:ChangeParkCharges(-1); end);      -- Dev CE Unit method
	local left = 0;
	pcall(function() left = unit:GetParkCharges(); end);
	if left <= 0 then
		pcall(function() UnitManager.Kill(unit); end);
	else
		pcall(function() UnitManager.FinishMoves(unit); end);
	end
	Reply(owner, "National Park created.");
end);

print("[DevCEPark][GP] ready");
