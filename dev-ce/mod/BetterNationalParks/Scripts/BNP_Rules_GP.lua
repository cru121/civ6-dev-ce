-- =====================================================================
--  BNP_Rules_GP.lua   (gameplay Lua context)
--
--  Reads the NATIONAL_PARK_DEFAULT rows of NationalParkTypes / NationalPark_ValidDistricts / NationalPark_ValidWonders and hands them to
--  the Dev CE native NationalParks.SetRules. From then on the game's own tile rule (vanilla Naturalist action, UI highlight, AI) uses them.
--  Optional extra rule in Lua (simulation thread only):
--    RegisterProcessor("NationalParkTileValid", function(map)   -- map.PlotIndex, map.PlayerId, map.Valid (0/1)
--        map.Valid = 0; map.Handled = 1; return true;            -- return a non-nil value, set Handled = 1 to override
--    end);
-- =====================================================================

local PARK_TYPE = "NATIONAL_PARK_DEFAULT";
local FLAG_IMPROVEMENTS, FLAG_MOUNTAINS, FLAG_NATURAL_WONDERS = 1, 2, 4;

local function Truthy(v) return v == true or v == 1 or v == "1" or v == "true"; end

local function PushRules()
	if NationalParks == nil or NationalParks.SetRules == nil then
		print("[BNP] Dev CE with NationalParks.SetRules is not loaded: the vanilla park rule stays");
		return;
	end
	local row = nil;
	for r in GameInfo.NationalParkTypes() do
		if r.ParkType == PARK_TYPE then row = r; break; end
	end
	if row == nil then print("[BNP] no row " .. PARK_TYPE .. ": the vanilla park rule stays"); return; end

	-- Setup-screen options (Config/BNP_Config.xml). Anything not set (nil) leaves the database rows in charge.
	local function Cfg(key) return GameConfiguration.GetValue(key); end
	local districtMode = Cfg("BNP_DISTRICT_MODE") or "DEFAULT";
	local wonderMode = Cfg("BNP_WONDER_MODE") or "DEFAULT";

	local districts = {};
	if districtMode == "CUSTOM" then
		local list = Cfg("BNP_DISTRICTS");
		if type(list) == "table" then
			for _, name in ipairs(list) do
				local d = GameInfo.Districts[(tostring(name):gsub("^BNP_D_", ""))];
				if d ~= nil then table.insert(districts, d.Index); else print("[BNP] unknown district " .. tostring(name)); end
			end
		end
	elseif districtMode ~= "NONE" then
		for r in GameInfo.NationalPark_ValidDistricts() do
			if r.ParkType == PARK_TYPE then
				local d = GameInfo.Districts[r.DistrictType];
				if d ~= nil then table.insert(districts, d.Index); else print("[BNP] unknown district " .. tostring(r.DistrictType)); end
			end
		end
	end

	local wonders = {};
	if wonderMode == "CUSTOM" then
		local list = Cfg("BNP_WONDERS");
		if type(list) == "table" then
			for _, name in ipairs(list) do
				local b = GameInfo.Buildings[(tostring(name):gsub("^BNP_W_", ""))];
				if b ~= nil then table.insert(wonders, b.Index); end      -- wonders of an unowned expansion are simply unknown here
			end
		end
	elseif wonderMode == "ALL" then
		for b in GameInfo.Buildings() do
			if Truthy(b.IsWonder) then table.insert(wonders, b.Index); end
		end
	elseif wonderMode ~= "NONE" then
		for r in GameInfo.NationalPark_ValidWonders() do
			if r.ParkType == PARK_TYPE then
				local b = GameInfo.Buildings[r.BuildingType];
				if b ~= nil then table.insert(wonders, b.Index); else print("[BNP] unknown wonder " .. tostring(r.BuildingType)); end
			end
		end
	end

	-- Custom shapes: with the option off the picker is hidden (UI and gameplay script check BNP_CUSTOM_SHAPE) and the size limits do not matter.
	local minTiles, maxTiles = row.MinTiles, row.MaxTiles;
	if Truthy(Cfg("BNP_CUSTOM_SHAPE")) then
		minTiles = tonumber(Cfg("BNP_MIN_SIZE")) or minTiles;
		maxTiles = tonumber(Cfg("BNP_MAX_SIZE")) or maxTiles;
		if maxTiles < minTiles then maxTiles = minTiles; end
	end

	local flags = 0;
	if Truthy(row.AllowImprovements) then flags = flags + FLAG_IMPROVEMENTS; end
	if Truthy(row.AllowMountains) then flags = flags + FLAG_MOUNTAINS; end
	if Truthy(row.AllowNaturalWonders) then flags = flags + FLAG_NATURAL_WONDERS; end

	NationalParks.SetRules(minTiles, maxTiles, row.MinAppeal, row.MaxAppeal, flags, districts, wonders);
	print("[BNP] rules pushed (districts " .. districtMode .. ", wonders " .. wonderMode .. ", custom shape " .. tostring(Cfg("BNP_CUSTOM_SHAPE")) .. "): tiles " .. minTiles .. ".." .. maxTiles .. ", appeal " .. row.MinAppeal .. ".." .. row.MaxAppeal
		.. ", flags " .. flags .. ", districts " .. #districts .. ", wonders " .. #wonders);
end

PushRules();
