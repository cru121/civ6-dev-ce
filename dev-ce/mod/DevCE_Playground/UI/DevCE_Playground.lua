-- =====================================================================
--  DevCE_Playground.lua   (runs in the UI Lua context)
--
--  An always-visible button panel. A button sends an EXECUTE_SCRIPT unit command
--  (PARAM_NAME = the command name) -> GameEvents.<name> in the gameplay VM, which does
--  the work (see Scripts/DevCE_Playground_Gameplay.lua). The pattern is the one used by the FreeGP mod.
--  The unit that carries the command is the selected unit (the charge buttons act on it), else the first unit.
--  Gameplay answers through two player properties (DEVCEPG_SEQ / DEVCEPG_MSG) that this script shows in the status line.
-- =====================================================================

print("[DevCEPg][UI] loading...");

-- control id, command name (= GameEvents name), status text
local BUTTONS = {
	{ "Btn_GoldenOn",     "DevCEPg_GoldenOn",     "Golden Age ON" },
	{ "Btn_GoldenOff",    "DevCEPg_GoldenOff",    "Golden Age OFF" },
	{ "Btn_DarkOn",       "DevCEPg_DarkOn",       "Dark Age ON" },
	{ "Btn_DarkOff",      "DevCEPg_DarkOff",      "Dark Age OFF" },
	{ "Btn_EraPlus",      "DevCEPg_EraPlus",      "Era +1" },
	{ "Btn_EraMinus",     "DevCEPg_EraMinus",     "Era -1" },
	{ "Btn_SpawnJames",   "DevCEPg_SpawnJames",   "Spawn James of St. George" },
	{ "Btn_VChargePlus",  "DevCEPg_VChargePlus",  "vanilla charge +1" },
	{ "Btn_VChargeMinus", "DevCEPg_VChargeMinus", "vanilla charge -1" },
	{ "Btn_DChargePlus",  "DevCEPg_DChargePlus",  "Dev CE charge +1" },
	{ "Btn_DChargeMinus", "DevCEPg_DChargeMinus", "Dev CE charge -1" },
	{ "Btn_FoodPlus",     "DevCEPg_FoodPlus",     "capital food +2" },
	{ "Btn_FoodMinus",    "DevCEPg_FoodMinus",    "capital food -2" },
	{ "Btn_RoutesPlus",   "DevCEPg_RoutesPlus",   "trade routes +1" },
	{ "Btn_RoutesMinus",  "DevCEPg_RoutesMinus",  "trade routes -1" },
	-- Sets B-I samples (one section in the panel; I eras reuses the 6 Era/Age buttons above)
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_BTokensRead",  "DevCEPg_BTokensRead",  "B: influence tokens readout" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_BTokensPlus",  "DevCEPg_BTokensPlus",  "B: influence tokens +1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_BTokensMinus", "DevCEPg_BTokensMinus", "B: influence tokens -1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_CMetRead",     "DevCEPg_CMetRead",     "C: majors met readout" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_CAllyPlus",    "DevCEPg_CAllyPlus",    "C: alliance points +1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_CAllyMinus",   "DevCEPg_CAllyMinus",   "C: alliance points -1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_DTourists",    "DevCEPg_DTourists",    "D: tourists readout" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_DParksMod",    "DevCEPg_DParksMod",    "D: parks tourism mod readout" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_DCivics",      "DevCEPg_DCivics",      "D: civics completed readout" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_EBuildProg",   "DevCEPg_EBuildProg",   "E: capital build progress" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_EProdPlus",    "DevCEPg_EProdPlus",    "E: capital production +1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_EProdMinus",   "DevCEPg_EProdMinus",   "E: capital production -1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_FGoldSet",     "DevCEPg_FGoldSet",     "F: gold rate SET +5 (one-way)" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_FGoldRestore", "DevCEPg_FGoldRestore", "F: gold rate restore 0" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_GScoreRead",   "DevCEPg_GScoreRead",   "G: era score readout" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_GScorePlus",   "DevCEPg_GScorePlus",   "G: era score +1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_GScoreMinus",  "DevCEPg_GScoreMinus",  "G: era score -1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_HParkPlus",    "DevCEPg_HParkPlus",    "H: park charges +1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_HParkMinus",   "DevCEPg_HParkMinus",   "H: park charges -1" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_HLevelPlus",   "DevCEPg_HLevelPlus",   "H: XP level +1 (one-way)" },
	-- retired 2026-10-18 (panel room for Terrain): { "Btn_R41Deal",      "DevCEPg_R41Deal",      "R41: deal check (negotiating)" },
	-- Terrain panel (left), handlers in Scripts/DevCE_Terrain_Gameplay.lua
	{ "Btn_T_Grass",       "DevCEPg_T_Grass",       "terrain Grass" },
	{ "Btn_T_Plains",      "DevCEPg_T_Plains",      "terrain Plains" },
	{ "Btn_T_Desert",      "DevCEPg_T_Desert",      "terrain Desert" },
	{ "Btn_T_Tundra",      "DevCEPg_T_Tundra",      "terrain Tundra" },
	{ "Btn_T_Snow",        "DevCEPg_T_Snow",        "terrain Snow" },
	{ "Btn_T_Coast",       "DevCEPg_T_Coast",       "terrain Coast" },
	{ "Btn_T_GrassHills",  "DevCEPg_T_GrassHills",  "terrain Grass hills" },
	{ "Btn_T_PlainsHills", "DevCEPg_T_PlainsHills", "terrain Plains hills" },
	{ "Btn_T_DesertHills", "DevCEPg_T_DesertHills", "terrain Desert hills" },
	{ "Btn_T_TundraHills", "DevCEPg_T_TundraHills", "terrain Tundra hills" },
	{ "Btn_T_SnowHills",   "DevCEPg_T_SnowHills",   "terrain Snow hills" },
	{ "Btn_T_GrassMtn",    "DevCEPg_T_GrassMtn",    "terrain Mountain" },
	{ "Btn_T_Ocean",       "DevCEPg_T_Ocean",       "terrain Ocean" },
	{ "Btn_T_Cycle",       "DevCEPg_T_Cycle",       "terrain: next in the cycle" },
	{ "Btn_T_HillSwap",    "DevCEPg_T_HillSwap",    "terrain: hills / flat swap" },
	{ "Btn_T_RingPlains",  "DevCEPg_T_RingPlains",  "terrain: ring of plains" },
	{ "Btn_T_ForestOn",    "DevCEPg_T_ForestOn",    "forest ON" },
	{ "Btn_T_ForestOff",   "DevCEPg_T_ForestOff",   "forest OFF" },
	{ "Btn_T_Readout",     "DevCEPg_T_Readout",     "tile readout" },
	{ "Btn_T_RingHills",   "DevCEPg_T_RingHills",   "terrain: ring of hills" },
	{ "Btn_T_FieldHills",  "DevCEPg_T_FieldHills",  "terrain: field of hills (radius 2)" },
	{ "Btn_T_RingFlat",    "DevCEPg_T_RingFlat",    "terrain: flatten hills (radius 2)" },
	{ "Btn_T_Undo",        "DevCEPg_T_Undo",        "terrain: undo last change" },
	-- Tile objects panel (left, second), handlers in Scripts/DevCE_TerrainObjects_Gameplay.lua
	{ "Btn_O_Jungle",      "DevCEPg_O_Jungle",      "feature Jungle" },
	{ "Btn_O_Marsh",       "DevCEPg_O_Marsh",       "feature Marsh" },
	{ "Btn_O_FeatOff",     "DevCEPg_O_FeatOff",     "feature OFF" },
	{ "Btn_O_R_Wheat",     "DevCEPg_O_R_Wheat",     "resource Wheat" },
	{ "Btn_O_R_Cattle",    "DevCEPg_O_R_Cattle",    "resource Cattle" },
	{ "Btn_O_R_Horses",    "DevCEPg_O_R_Horses",    "resource Horses" },
	{ "Btn_O_R_Copper",    "DevCEPg_O_R_Copper",    "resource Copper" },
	{ "Btn_O_R_Iron",      "DevCEPg_O_R_Iron",      "resource Iron" },
	{ "Btn_O_R_Stone",     "DevCEPg_O_R_Stone",     "resource Stone" },
	{ "Btn_O_R_Off",       "DevCEPg_O_R_Off",       "resource OFF" },
	{ "Btn_O_I_Farm",      "DevCEPg_O_I_Farm",      "improvement Farm" },
	{ "Btn_O_I_Mine",      "DevCEPg_O_I_Mine",      "improvement Mine" },
	{ "Btn_O_I_Quarry",    "DevCEPg_O_I_Quarry",    "improvement Quarry" },
	{ "Btn_O_I_Pasture",   "DevCEPg_O_I_Pasture",   "improvement Pasture" },
	{ "Btn_O_I_Camp",      "DevCEPg_O_I_Camp",      "improvement Camp" },
	{ "Btn_O_I_Off",       "DevCEPg_O_I_Off",       "improvement OFF" },
};

local FRAME_HEIGHT = 520;   -- must match Pg_Frame height in the XML
local FRAME2_HEIGHT = 710;  -- must match Pg_Frame2 (left Terrain panel) height in the XML
local FRAME3_HEIGHT = 500;  -- must match Pg_Frame3 (Tile objects panel) height in the XML
local m_collapsed = false;
local m_lastSeq = nil;

local function FirstUnit(playerID)
	local found = nil;
	pcall(function()
		for _, u in Players[playerID]:GetUnits():Members() do found = u; break; end
	end);
	return found;
end

local function Send(command, text)
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then Controls.Pg_Status:SetText("No local player."); return; end

	local unit = UI.GetHeadSelectedUnit();
	if unit == nil then unit = FirstUnit(pid); end
	if unit == nil then Controls.Pg_Status:SetText("You need at least one unit to send a command."); return; end

	local params = {};
	params[UnitCommandTypes.PARAM_NAME] = command;
	params[UnitCommandTypes.PARAM_X] = 0;
	params[UnitCommandTypes.PARAM_Y] = 0;
	local ok = pcall(function() UnitManager.RequestCommand(unit, UnitCommandTypes.EXECUTE_SCRIPT, params); end);
	Controls.Pg_Status:SetText(ok and ("Sent: " .. text) or ("Could not send: " .. text));
	print("[DevCEPg][UI] sent " .. command .. " (carrier unit " .. tostring(unit:GetID()) .. ") ok=" .. tostring(ok));
end


-- ---- UI-cache terrain under the selected unit (compare with what the 3D map shows)
local function UpdateTerrainCache()
	local text = "Tile under unit (UI cache): no unit";
	pcall(function()
		local u = UI.GetHeadSelectedUnit();
		if u == nil then
			local pid = Game.GetLocalPlayer();
			if pid ~= nil and pid >= 0 then u = FirstUnit(pid); end
		end
		if u ~= nil then
			local plot = Map.GetPlot(u:GetX(), u:GetY());
			local t = plot:GetTerrainType();
			local row = GameInfo.Terrains[t];
			local f = plot:GetFeatureType();
			local frow = (f ~= nil and f >= 0) and GameInfo.Features[f] or nil;
			text = "Tile " .. u:GetX() .. "," .. u:GetY() .. " (UI cache): " .. (row and row.TerrainType:gsub("TERRAIN_", "") or tostring(t)) .. (frow and ("  + " .. frow.FeatureType:gsub("FEATURE_", "")) or "");
		end
	end);
	Controls.Pg_TerrCache:SetText(text);
end

-- ---- quick save / quick load (what the quicksave hotkeys do): the load rebuilds the whole world view, so terrain changes show up
local m_quickLoadQuery = nil;
local function QuickSave()
	local ok, err = pcall(function()
		local gameFile = {};
		gameFile.Name = "quicksave";
		gameFile.Location = SaveLocations.LOCAL_STORAGE;
		gameFile.Type = Network.GetGameConfigurationSaveType();
		gameFile.IsAutosave = false;
		gameFile.IsQuicksave = true;
		Network.SaveGame(gameFile);
	end);
	Controls.Pg_Status:SetText(ok and "Quick save requested. Wait a moment, then Quick load." or ("Quick save failed: " .. tostring(err)));
end
local function OnQuickLoadResults(fileList, queryID)
	if m_quickLoadQuery == nil or m_quickLoadQuery ~= queryID then return; end
	m_quickLoadQuery = nil;
	if fileList == nil or #fileList == 0 then Controls.Pg_Status:SetText("No quicksave found. Press Quick save first."); return; end
	local save = fileList[1];
	local ok, err = pcall(function()
		local errors = Modding.CheckRequirements(save.RequiredMods or {}, SaveTypes.SINGLE_PLAYER);
		if errors == nil or errors.Success then
			Network.LoadGame(save, ServerType.SERVER_TYPE_NONE);
		else
			error("the quicksave needs other mods than the ones enabled now");
		end
	end);
	if not ok then Controls.Pg_Status:SetText("Quick load failed: " .. tostring(err)); end
	pcall(function() UI.CloseFileListQuery(queryID); end);
end
local function QuickLoad()
	local ok, err = pcall(function()
		m_quickLoadQuery = UI.QuerySaveGameList(SaveLocations.LOCAL_STORAGE, SaveTypes.SINGLE_PLAYER, SaveLocationOptions.QUICKSAVE + SaveLocationOptions.LOAD_METADATA);
	end);
	Controls.Pg_Status:SetText(ok and "Looking for the quicksave..." or ("Quick load failed: " .. tostring(err) .. " (the game's own quickload hotkey still works)"));
end

-- The gameplay VM reports what it did in player properties.
local function OnUpdate()
	-- The game creates additional contexts hidden (the log said "context hidden=true"): keep this one shown.
	if ContextPtr:IsHidden() then ContextPtr:SetHide(false); end
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then return; end
	pcall(function()
		local eras = Game.GetEras();
		Controls.Pg_Ages:SetText("Ages:  Golden " .. tostring(eras:HasGoldenAge(pid)) .. "   Dark " .. tostring(eras:HasDarkAge(pid)) .. "   Heroic " .. tostring(eras:HasHeroicGoldenAge(pid)));
		local e = eras:GetCurrentEra();
		local row = GameInfo.Eras[e];
		Controls.Pg_Era:SetText("Era: " .. tostring(e) .. " " .. (row and row.EraType or "?") .. "   started turn " .. tostring(eras:GetCurrentEraStartTurn()));
	end);
	UpdateTerrainCache();
	local seq, msg;
	pcall(function()
		seq = Players[pid]:GetProperty("DEVCEPG_SEQ");
		msg = Players[pid]:GetProperty("DEVCEPG_MSG");
	end);
	if seq ~= nil and seq ~= m_lastSeq then
		m_lastSeq = seq;
		if msg ~= nil then Controls.Pg_Status:SetText(tostring(msg)); end
	end
end

local function Toggle()
	m_collapsed = not m_collapsed;
	Controls.Pg_Body:SetHide(m_collapsed);
	Controls.Pg_Toggle:SetText(m_collapsed and "Dev CE Playground  [+]" or "Dev CE Playground  [-]");
	Controls.Pg_Outer:CalculateSize();
	Controls.Pg_Outer2:CalculateSize();
	Controls.Pg_Frame:SetSizeY(m_collapsed and 52 or FRAME_HEIGHT);
end

local m_collapsed2 = false;
local function Toggle2()
	m_collapsed2 = not m_collapsed2;
	Controls.Pg_Body2:SetHide(m_collapsed2);
	Controls.Pg_Toggle2:SetText(m_collapsed2 and "Terrain  [+]" or "Terrain  [-]");
	Controls.Pg_Outer2:CalculateSize();
	Controls.Pg_Frame2:SetSizeY(m_collapsed2 and 52 or FRAME2_HEIGHT);
end

local m_collapsed3 = false;
local function Toggle3()
	m_collapsed3 = not m_collapsed3;
	Controls.Pg_Body3:SetHide(m_collapsed3);
	Controls.Pg_Toggle3:SetText(m_collapsed3 and "Tile objects  [+]" or "Tile objects  [-]");
	Controls.Pg_Outer3:CalculateSize();
	Controls.Pg_Frame3:SetSizeY(m_collapsed3 and 52 or FRAME3_HEIGHT);
end

local function Initialize()
	for _, b in ipairs(BUTTONS) do
		local ctrl, command, text = b[1], b[2], b[3];
		Controls[ctrl]:RegisterCallback(Mouse.eLClick, function() Send(command, text); end);
	end
	Controls.Btn_T_QuickSave:RegisterCallback(Mouse.eLClick, QuickSave);
	Controls.Btn_T_QuickLoad:RegisterCallback(Mouse.eLClick, QuickLoad);
	if LuaEvents.FileListQueryResults ~= nil then LuaEvents.FileListQueryResults.Add(OnQuickLoadResults); end
	Controls.Pg_Toggle:RegisterCallback(Mouse.eLClick, Toggle);
	Controls.Pg_Toggle2:RegisterCallback(Mouse.eLClick, Toggle2);
	Controls.Pg_Toggle3:RegisterCallback(Mouse.eLClick, Toggle3);
	ContextPtr:SetHide(false);
	ContextPtr:SetUpdate(OnUpdate);
	if Events.LoadScreenClose ~= nil then Events.LoadScreenClose.Add(function() ContextPtr:SetHide(false); end); end
	Controls.Pg_Outer:CalculateSize();
	Controls.Pg_Outer2:CalculateSize();
	Controls.Pg_Outer3:CalculateSize();
	pcall(function()
		local sx, sy = UIManager:GetScreenSizeVal();
		print(string.format("[DevCEPg][UI] ready. screen %sx%s; frame size %sx%s offset %s,%s hidden=%s; context hidden=%s",
			tostring(sx), tostring(sy), tostring(Controls.Pg_Frame:GetSizeX()), tostring(Controls.Pg_Frame:GetSizeY()),
			tostring(Controls.Pg_Frame:GetOffsetX()), tostring(Controls.Pg_Frame:GetOffsetY()), tostring(Controls.Pg_Frame:IsHidden()), tostring(ContextPtr:IsHidden())));
	end);
end

Initialize();
