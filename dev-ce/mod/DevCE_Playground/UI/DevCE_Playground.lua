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
	{ "Btn_BTokensRead",  "DevCEPg_BTokensRead",  "B: influence tokens readout" },
	{ "Btn_BTokensPlus",  "DevCEPg_BTokensPlus",  "B: influence tokens +1" },
	{ "Btn_BTokensMinus", "DevCEPg_BTokensMinus", "B: influence tokens -1" },
	{ "Btn_CMetRead",     "DevCEPg_CMetRead",     "C: majors met readout" },
	{ "Btn_CAllyPlus",    "DevCEPg_CAllyPlus",    "C: alliance points +1" },
	{ "Btn_CAllyMinus",   "DevCEPg_CAllyMinus",   "C: alliance points -1" },
	{ "Btn_DTourists",    "DevCEPg_DTourists",    "D: tourists readout" },
	{ "Btn_DParksMod",    "DevCEPg_DParksMod",    "D: parks tourism mod readout" },
	{ "Btn_DCivics",      "DevCEPg_DCivics",      "D: civics completed readout" },
	{ "Btn_EBuildProg",   "DevCEPg_EBuildProg",   "E: capital build progress" },
	{ "Btn_EProdPlus",    "DevCEPg_EProdPlus",    "E: capital production +1" },
	{ "Btn_EProdMinus",   "DevCEPg_EProdMinus",   "E: capital production -1" },
	{ "Btn_FGoldSet",     "DevCEPg_FGoldSet",     "F: gold rate SET +5 (one-way)" },
	{ "Btn_FGoldRestore", "DevCEPg_FGoldRestore", "F: gold rate restore 0" },
	{ "Btn_GScoreRead",   "DevCEPg_GScoreRead",   "G: era score readout" },
	{ "Btn_GScorePlus",   "DevCEPg_GScorePlus",   "G: era score +1" },
	{ "Btn_GScoreMinus",  "DevCEPg_GScoreMinus",  "G: era score -1" },
	{ "Btn_HParkPlus",    "DevCEPg_HParkPlus",    "H: park charges +1" },
	{ "Btn_HParkMinus",   "DevCEPg_HParkMinus",   "H: park charges -1" },
	{ "Btn_HLevelPlus",   "DevCEPg_HLevelPlus",   "H: XP level +1 (one-way)" },
	{ "Btn_R41Deal",      "DevCEPg_R41Deal",      "R41: deal check (negotiating)" },
};

local FRAME_HEIGHT = 820;   -- must match Pg_Frame height in the XML
local FRAME2_HEIGHT = 400;  -- must match Pg_Frame2 (left E-H column) height in the XML
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
	Controls.Pg_Toggle2:SetText(m_collapsed2 and "Sets E-H samples  [+]" or "Sets E-H samples  [-]");
	Controls.Pg_Outer2:CalculateSize();
	Controls.Pg_Frame2:SetSizeY(m_collapsed2 and 52 or FRAME2_HEIGHT);
end

local function Initialize()
	for _, b in ipairs(BUTTONS) do
		local ctrl, command, text = b[1], b[2], b[3];
		Controls[ctrl]:RegisterCallback(Mouse.eLClick, function() Send(command, text); end);
	end
	Controls.Pg_Toggle:RegisterCallback(Mouse.eLClick, Toggle);
	Controls.Pg_Toggle2:RegisterCallback(Mouse.eLClick, Toggle2);
	ContextPtr:SetHide(false);
	ContextPtr:SetUpdate(OnUpdate);
	if Events.LoadScreenClose ~= nil then Events.LoadScreenClose.Add(function() ContextPtr:SetHide(false); end); end
	Controls.Pg_Outer:CalculateSize();
	Controls.Pg_Outer2:CalculateSize();
	pcall(function()
		local sx, sy = UIManager:GetScreenSizeVal();
		print(string.format("[DevCEPg][UI] ready. screen %sx%s; frame size %sx%s offset %s,%s hidden=%s; context hidden=%s",
			tostring(sx), tostring(sy), tostring(Controls.Pg_Frame:GetSizeX()), tostring(Controls.Pg_Frame:GetSizeY()),
			tostring(Controls.Pg_Frame:GetOffsetX()), tostring(Controls.Pg_Frame:GetOffsetY()), tostring(Controls.Pg_Frame:IsHidden()), tostring(ContextPtr:IsHidden())));
	end);
end

Initialize();
