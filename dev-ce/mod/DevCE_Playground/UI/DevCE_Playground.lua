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
	{ "Btn_SpawnJames",   "DevCEPg_SpawnJames",   "Spawn James of St. George" },
	{ "Btn_VChargePlus",  "DevCEPg_VChargePlus",  "vanilla charge +1" },
	{ "Btn_VChargeMinus", "DevCEPg_VChargeMinus", "vanilla charge -1" },
	{ "Btn_DChargePlus",  "DevCEPg_DChargePlus",  "Dev CE charge +1" },
	{ "Btn_DChargeMinus", "DevCEPg_DChargeMinus", "Dev CE charge -1" },
	{ "Btn_FoodPlus",     "DevCEPg_FoodPlus",     "capital food +2" },
	{ "Btn_FoodMinus",    "DevCEPg_FoodMinus",    "capital food -2" },
	{ "Btn_RoutesPlus",   "DevCEPg_RoutesPlus",   "trade routes +1" },
	{ "Btn_RoutesMinus",  "DevCEPg_RoutesMinus",  "trade routes -1" },
};

local FRAME_HEIGHT = 450;   -- must match the Grid's height in the XML
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
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then return; end
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
	Controls.Pg_Frame:SetSizeY(m_collapsed and 52 or FRAME_HEIGHT);
end

local function Initialize()
	for _, b in ipairs(BUTTONS) do
		local ctrl, command, text = b[1], b[2], b[3];
		Controls[ctrl]:RegisterCallback(Mouse.eLClick, function() Send(command, text); end);
	end
	Controls.Pg_Toggle:RegisterCallback(Mouse.eLClick, Toggle);
	ContextPtr:SetUpdate(OnUpdate);
	Controls.Pg_Outer:CalculateSize();
	pcall(function()
		local sx, sy = UIManager:GetScreenSizeVal();
		print(string.format("[DevCEPg][UI] ready. screen %sx%s; frame size %sx%s offset %s,%s hidden=%s; context hidden=%s",
			tostring(sx), tostring(sy), tostring(Controls.Pg_Frame:GetSizeX()), tostring(Controls.Pg_Frame:GetSizeY()),
			tostring(Controls.Pg_Frame:GetOffsetX()), tostring(Controls.Pg_Frame:GetOffsetY()), tostring(Controls.Pg_Frame:IsHidden()), tostring(ContextPtr:IsHidden())));
	end);
end

Initialize();
