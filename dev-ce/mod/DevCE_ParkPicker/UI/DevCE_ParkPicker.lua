-- =====================================================================
--  DevCE_ParkPicker.lua   (UI Lua context)
--
--  A second action button for the Naturalist: "Designate custom National Park". The player picks 4 tiles one by one
--  (green = can be picked, yellow = already picked); the 4th click sends the choice to the gameplay script, which calls the
--  Dev CE native NationalParks.DesignateCustom. The vanilla "Designate National Park" operation is untouched (and so is the AI).
--
--  Tile rules (in this file and re-checked in Scripts/DevCE_ParkPicker_GP.lua):
--    * every tile must satisfy the game's own park rule (NationalParks.IsValidParkTile: owner, no improvement, no district,
--      mountain / natural wonder / appeal, not already in a park)
--    * all four tiles belong to the same city (as in the vanilla rule)
--    * the first tile is within PICK_RADIUS of the Naturalist, later tiles touch an already picked tile and stay within MAX_RADIUS
--  Tune the constants below.
-- =====================================================================

local CMD_NAME    = "DEVCEPARK_DESIGNATE";
local TILE_COUNT  = 4;
local PICK_RADIUS = 2;     -- first tile: at most this far from the Naturalist
local MAX_RADIUS  = 4;     -- every tile: at most this far from the Naturalist
local BASE        = 4096;  -- plot indices are packed 2 per command parameter (index < 4096)


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

local LAYER_CANDIDATES = UILens.CreateLensLayerHash("Hex_Coloring_Movement");
local LAYER_PICKED     = UILens.CreateLensLayerHash("Hex_Coloring_Attack");

local m_picking   = false;
local m_unitID    = nil;
local m_picked    = {};
local m_candidates = {};
local m_sentSeq   = nil;   -- waiting for the gameplay answer
local m_waited    = 0;

-- ---------------------------------------------------------------------
local function PlayerID() return Game.GetLocalPlayer(); end

local function GetUnit()
	local pUnit = UI.GetHeadSelectedUnit();
	return pUnit;
end

local function Say(text, x, y)
	print("[DevCEPark][UI] " .. text);
	pcall(function() UI.AddWorldViewText(PlayerID(), text, x or 0, y or 0); end);
end

local function OwningCityKey(plotIndex)
	local key = nil;
	pcall(function()
		local city = Cities.GetPlotPurchaseCity(plotIndex);
		if city ~= nil then key = city:GetOwner() .. ":" .. city:GetID(); end
	end);
	return key;   -- nil = unknown (rule skipped)
end

local function IsValid(plotIndex)
	local ok, result = pcall(ParkTileValid, PlayerID(), plotIndex);
	return ok and result == true;
end

local function IsPicked(plotIndex)
	for _, p in ipairs(m_picked) do if p == plotIndex then return true; end end
	return false;
end

local function Distance(plotIndex, unit)
	local x, y = Map.GetPlotLocation(plotIndex);
	return Map.GetPlotDistance(x, y, unit:GetX(), unit:GetY());
end

local function Adjacent(a, b)
	local ax, ay = Map.GetPlotLocation(a);
	local bx, by = Map.GetPlotLocation(b);
	return Map.GetPlotDistance(ax, ay, bx, by) == 1;
end

local function ComputeCandidates(unit)
	local list = {};
	local seen = {};
	local function consider(plotIndex)
		if seen[plotIndex] or IsPicked(plotIndex) then return; end
		seen[plotIndex] = true;
		if Distance(plotIndex, unit) > MAX_RADIUS then return; end
		if not IsValid(plotIndex) then return; end
		if #m_picked > 0 then
			local firstCity = OwningCityKey(m_picked[1]);
			local city = OwningCityKey(plotIndex);
			if firstCity ~= nil and city ~= nil and firstCity ~= city then return; end
		end
		table.insert(list, plotIndex);
	end
	if #m_picked == 0 then
		for _, plot in ipairs(Map.GetNeighborPlots(unit:GetX(), unit:GetY(), PICK_RADIUS)) do consider(plot:GetIndex()); end
		consider(Map.GetPlotIndex(unit:GetX(), unit:GetY()));
	else
		for _, picked in ipairs(m_picked) do
			local x, y = Map.GetPlotLocation(picked);
			for dir = 0, 5 do
				local n = Map.GetAdjacentPlot(x, y, dir);
				if n ~= nil then consider(n:GetIndex()); end
			end
		end
	end
	return list;
end

local function Redraw()
	local pid = PlayerID();
	UILens.ClearLayerHexes(LAYER_CANDIDATES);
	UILens.ClearLayerHexes(LAYER_PICKED);
	if #m_candidates > 0 then
		UILens.SetLayerHexesColoredArea(LAYER_CANDIDATES, pid, m_candidates, UI.GetColorValue("COLOR_GREEN"));
		UILens.ToggleLayerOn(LAYER_CANDIDATES);
	else
		UILens.ToggleLayerOff(LAYER_CANDIDATES);
	end
	if #m_picked > 0 then
		UILens.SetLayerHexesColoredArea(LAYER_PICKED, pid, m_picked, UI.GetColorValue("COLOR_YELLOW"));
		UILens.ToggleLayerOn(LAYER_PICKED);
	else
		UILens.ToggleLayerOff(LAYER_PICKED);
	end
end

local function Stop(changeMode)
	m_picking = false;
	m_picked = {};
	m_candidates = {};
	pcall(function() UILens.ClearLayerHexes(LAYER_CANDIDATES); UILens.ToggleLayerOff(LAYER_CANDIDATES); end);
	pcall(function() UILens.ClearLayerHexes(LAYER_PICKED); UILens.ToggleLayerOff(LAYER_PICKED); end);
	Controls.DCP_Button:SetSelected(false);
	if changeMode then UI.SetInterfaceMode(InterfaceModeTypes.SELECTION); end
end

-- ---------------------------------------------------------------------
local function CanOffer(unit)
	if unit == nil or unit:GetOwner() ~= PlayerID() then return false; end
	if NationalParks == nil or NationalParks.IsValidParkTile == nil then return false; end   -- needs Dev CE
	local info = GameInfo.Units[unit:GetType()];
	if info == nil or (info.ParkCharges or 0) <= 0 then return false; end
	if unit:GetParkCharges() <= 0 then return false; end
	if unit:GetMovementMovesRemaining() <= 0 then return false; end
	return true;
end

local function RefreshButton()
	local unit = GetUnit();
	Controls.DCP_ButtonGrid:SetHide(not CanOffer(unit));
end

local function Send(unit)
	local pid = PlayerID();
	local prev = 0;
	pcall(function() prev = tonumber(Players[pid]:GetProperty("DEVCEPARK_SEQ")) or 0; end);
	m_sentSeq = prev;     -- the gameplay script increments the property when it answers
	m_waited = 0;
	local params = {};
	params[UnitCommandTypes.PARAM_NAME] = CMD_NAME;
	params[UnitCommandTypes.PARAM_X] = m_picked[1] * BASE + m_picked[2];
	params[UnitCommandTypes.PARAM_Y] = m_picked[3] * BASE + m_picked[4];
	local ok = pcall(function() UnitManager.RequestCommand(unit, UnitCommandTypes.EXECUTE_SCRIPT, params); end);
	print("[DevCEPark][UI] sent " .. table.concat(m_picked, ",") .. " ok=" .. tostring(ok));
	if not ok then m_sentSeq = nil; Say("Could not send the park request."); end
end

local function OnPlotClicked(plotIndex, plotEdge, isDown, isRightButton)
	if not m_picking or isDown then return; end
	local unit = GetUnit();
	if unit == nil or unit:GetID() ~= m_unitID then Stop(true); return; end

	if isRightButton then
		if #m_picked > 0 then
			table.remove(m_picked);
			m_candidates = ComputeCandidates(unit);
			Redraw();
		else
			Stop(true);
		end
		return;
	end

	local allowed = false;
	for _, c in ipairs(m_candidates) do if c == plotIndex then allowed = true; break; end end
	if not allowed then return; end

	table.insert(m_picked, plotIndex);
	if #m_picked >= TILE_COUNT then
		Send(unit);
		Stop(true);
		return;
	end
	m_candidates = ComputeCandidates(unit);
	if #m_candidates == 0 then
		local x, y = Map.GetPlotLocation(plotIndex);
		Say("No more tiles can be added here. Right-click to undo the last tile.", x, y);
	end
	Redraw();
end

local function OnButtonClicked()
	if m_picking then Stop(true); return; end
	local unit = GetUnit();
	if not CanOffer(unit) then RefreshButton(); return; end
	m_unitID = unit:GetID();
	m_picked = {};
	m_candidates = ComputeCandidates(unit);
	if #m_candidates == 0 then
		Say("No tile near the Naturalist can be part of a National Park.", unit:GetX(), unit:GetY());
		return;
	end
	UI.SetInterfaceMode(InterfaceModeTypes.SELECTION);
	UI.SetInterfaceMode(InterfaceModeTypes.WB_SELECT_PLOT);
	m_picking = true;
	Controls.DCP_Button:SetSelected(true);
	Redraw();
end

local function OnInterfaceModeChanged(oldMode, newMode)
	if m_picking and newMode ~= InterfaceModeTypes.WB_SELECT_PLOT then Stop(false); end
end

-- gameplay answer: player property DEVCEPARK_SEQ changes, DEVCEPARK_MSG holds the text
local function OnUpdate(delta)
	if m_sentSeq == nil then return; end
	m_waited = m_waited + delta;
	local pid = PlayerID();
	local seq = nil;
	pcall(function() seq = tonumber(Players[pid]:GetProperty("DEVCEPARK_SEQ")); end);
	if seq ~= nil and seq ~= m_sentSeq then
		local msg = nil;
		pcall(function() msg = Players[pid]:GetProperty("DEVCEPARK_MSG"); end);
		m_sentSeq = nil;
		local unit = GetUnit();
		Say(msg or "National Park request answered.", unit and unit:GetX() or 0, unit and unit:GetY() or 0);
		RefreshButton();
	elseif m_waited > 5 then
		m_sentSeq = nil;
		Say("No answer from the gameplay script (is Dev CE loaded?).");
	end
end

-- ---------------------------------------------------------------------
local function Initialize()
	local stack = ContextPtr:LookUpControl("/InGame/UnitPanel/StandardActionsStack");
	if stack ~= nil then Controls.DCP_ButtonGrid:ChangeParent(stack); end
	Controls.DCP_Button:RegisterCallback(Mouse.eLClick, OnButtonClicked);
	ContextPtr:SetUpdate(OnUpdate);

	LuaEvents.WorldInput_WBSelectPlot.Add(OnPlotClicked);
	Events.InterfaceModeChanged.Add(OnInterfaceModeChanged);
	Events.UnitSelectionChanged.Add(function(playerID, unitID, x, y, z, isSelected, isEditable)
		if m_picking then Stop(true); end
		RefreshButton();
	end);
	pcall(function() Events.UnitMoveComplete.Add(function() RefreshButton(); end); end);
	pcall(function() Events.UnitCommandStarted.Add(function() RefreshButton(); end); end);
	pcall(function() Events.UnitChargesChanged.Add(function() RefreshButton(); end); end);
	RefreshButton();
	print("[DevCEPark][UI] ready (button shows for Naturalists when Dev CE is loaded)");
end

Events.LoadGameViewStateDone.Add(Initialize);
