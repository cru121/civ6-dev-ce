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

	local districts = {};
	for r in GameInfo.NationalPark_ValidDistricts() do
		if r.ParkType == PARK_TYPE then
			local d = GameInfo.Districts[r.DistrictType];
			if d ~= nil then table.insert(districts, d.Index); else print("[BNP] unknown district " .. tostring(r.DistrictType)); end
		end
	end

	local wonders = {};
	for r in GameInfo.NationalPark_ValidWonders() do
		if r.ParkType == PARK_TYPE then
			local b = GameInfo.Buildings[r.BuildingType];
			if b ~= nil then table.insert(wonders, b.Index); else print("[BNP] unknown wonder " .. tostring(r.BuildingType)); end
		end
	end

	local flags = 0;
	if Truthy(row.AllowImprovements) then flags = flags + FLAG_IMPROVEMENTS; end
	if Truthy(row.AllowMountains) then flags = flags + FLAG_MOUNTAINS; end
	if Truthy(row.AllowNaturalWonders) then flags = flags + FLAG_NATURAL_WONDERS; end

	NationalParks.SetRules(row.MinTiles, row.MaxTiles, row.MinAppeal, row.MaxAppeal, flags, districts, wonders);
	print("[BNP] rules pushed: tiles " .. row.MinTiles .. ".." .. row.MaxTiles .. ", appeal " .. row.MinAppeal .. ".." .. row.MaxAppeal
		.. ", flags " .. flags .. ", districts " .. #districts .. ", wonders " .. #wonders);
end

PushRules();
