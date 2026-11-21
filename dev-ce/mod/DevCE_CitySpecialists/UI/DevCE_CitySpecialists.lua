-- ===========================================================================
--	Dev CE city specialists: the specialist slots on the city centre in the citizen management lens.
--	One small (32x32) citizen button per type in DevCE_Specialists, tinted with the type's Color (a UI colour name or "R,G,B"); slot
--	counts are in the tooltip. Rows of 3 from the top of the city centre's own citizen button, centred on it, never above it. Reads the city properties
--	(DEVCE_SPECIALIST_SLOTS_<Type>, DEVCE_SPECIALISTS_<Type>, DEVCE_SPECIALISTS_LOCKED_<Type>). Left-click locks one more citizen into
--	the type, right-click removes a lock; the request goes to Scripts/DevCE_CitySpecialists_GP.lua through EXECUTE_SCRIPT.
-- ===========================================================================
include("InstanceManager");

local MODE_PARAMETER	:string = "DEVCE_CITY_CENTER_SPECIALISTS";
local BUTTON_SIZE		:number = 32;
local PITCH_X			:number = 38;	-- button 32 + gap
local PITCH_Y			:number = 36;
local PER_ROW			:number = 3;
local FRAME_EMPTY		:number = 0;	-- rows of the citizen button textures (32 px each), as PlotInfo uses the 64 px ones
local FRAME_FILLED		:number = BUTTON_SIZE * 4;
local POLL_SECONDS		:number = 0.25;

local m_CitizenManagement	:number = UILens.CreateLensLayerHash("Citizen_Management");
local m_SlotIM				:table = InstanceManager:new("SlotInstance", "Root", Controls.SlotContainer);
local m_kTypes				:table = {};	-- { Type, Name, Color, Yields }
local m_bEnabled			:boolean = false;
local m_pCity				:table = nil;	-- the city shown
local m_sLast				:string = "";	-- snapshot of the shown values
local m_fSince				:number = 0;

-- ===========================================================================
-- A UI colour name (UI.GetColorValue) or "R,G,B" / "R,G,B,A" (0-255); nil = no tint.
function ParseColor(text)
	if text == nil or text == "" then return nil; end
	local r, g, b, a = string.match(text, "^%s*(%d+)%s*,%s*(%d+)%s*,%s*(%d+)%s*,?%s*(%d*)%s*$");
	if r ~= nil then
		return UI.GetColorValue(tonumber(r) / 255, tonumber(g) / 255, tonumber(b) / 255, (a ~= "" and tonumber(a) or 255) / 255);
	end
	local value = UI.GetColorValue(text);	-- no value at all for an unknown name
	return value;
end

-- Offset of slot i (1-based) of n: rows of PER_ROW, each row centred; the first row's middle slot at 0,0 (the city centre's button).
local function SlotOffset(i:number, n:number)
	local row:number = math.floor((i - 1) / PER_ROW);
	local inRow:number = math.min(PER_ROW, n - row * PER_ROW);
	local col:number = (i - 1) % PER_ROW;
	return math.floor((col - (inRow - 1) / 2) * PITCH_X + 0.5), row * PITCH_Y;
end

-- ===========================================================================
local function ReadSetup()
	m_bEnabled = false;
	for row in GameInfo.GlobalParameters() do
		if row.Name == MODE_PARAMETER then m_bEnabled = tonumber(row.Value) == 1; end
	end
	m_kTypes = {};
	if not m_bEnabled or GameInfo.DevCE_Specialists == nil then m_bEnabled = false; return; end
	for row in GameInfo.DevCE_Specialists() do
		table.insert(m_kTypes, { Type = row.SpecialistType, Name = row.Name, Color = ParseColor(row.Color), Sort = row.SortIndex or 0, Yields = {}, Points = {} });
	end
	table.sort(m_kTypes, function(a, b) if a.Sort ~= b.Sort then return a.Sort < b.Sort; end return a.Type < b.Type; end);
	if GameInfo.DevCE_Specialist_Yields ~= nil then
		for row in GameInfo.DevCE_Specialist_Yields() do
			for _, t in ipairs(m_kTypes) do
				if t.Type == row.SpecialistType then t.Yields[row.YieldType] = (t.Yields[row.YieldType] or 0) + row.YieldChange; end
			end
		end
	end
	if GameInfo.DevCE_Specialist_GreatPersonPoints ~= nil then
		for row in GameInfo.DevCE_Specialist_GreatPersonPoints() do
			local class = GameInfo.GreatPersonClasses[row.GreatPersonClassType];
			for _, t in ipairs(m_kTypes) do
				if t.Type == row.SpecialistType and class ~= nil then
					table.insert(t.Points, string.format("%+d %s %s", row.PointsPerTurn, class.IconString or "", Locale.Lookup(class.Name)));
				end
			end
		end
	end
	if #m_kTypes == 0 then m_bEnabled = false; end
end

local function Get(pCity:table, key:string)
	local value = pCity:GetProperty(key);	-- returns no value at all for a key never set
	return tonumber(value) or 0;
end

local function Values(pCity:table, t:table)
	local slots		:number = math.max(0, Get(pCity, "DEVCE_SPECIALIST_SLOTS_" .. t.Type));
	local working	:number = math.min(Get(pCity, "DEVCE_SPECIALISTS_" .. t.Type), slots);
	local locked	:number = Get(pCity, "DEVCE_SPECIALISTS_LOCKED_" .. t.Type);
	return slots, working, locked;
end

-- What one specialist of type t gives in this city: the table plus DEVCE_SPECIALIST_YIELD_<Type>_<YieldType>, then great person points.
local function PerSpecialistText(pCity:table, t:table)
	local parts:table = {};
	for row in GameInfo.Yields() do
		local value:number = (t.Yields[row.YieldType] or 0) + Get(pCity, "DEVCE_SPECIALIST_YIELD_" .. t.Type .. "_" .. row.YieldType);
		if value ~= 0 then table.insert(parts, string.format("%+d %s", value, row.IconString or row.YieldType)); end
	end
	for _, text in ipairs(t.Points) do table.insert(parts, text); end
	return table.concat(parts, ", ");
end

local function Request(pCity:table, t:table, count:number)
	local kParameters:table = {};
	kParameters.OnStart = "DevCE_SetSpecialistLock";
	kParameters.CityID = pCity:GetID();
	kParameters.SpecialistType = t.Type;
	kParameters.Count = count;
	UI.RequestPlayerOperation(Game.GetLocalPlayer(), PlayerOperations.EXECUTE_SCRIPT, kParameters);
end

-- ===========================================================================
local function Refresh()
	m_SlotIM:ResetInstances();
	local pCity:table = m_pCity;
	if pCity == nil then return; end
	local n:number = #m_kTypes;
	local snapshot:table = {};
	for i, t in ipairs(m_kTypes) do
		local slots:number, working:number, locked:number = Values(pCity, t);
		table.insert(snapshot, slots .. "/" .. working .. "/" .. locked);

		local inst:table = m_SlotIM:GetInstance();
		local x:number, y:number = SlotOffset(i, n);
		inst.Root:SetOffsetVal(200 + x - BUTTON_SIZE / 2, y);	-- SlotContainer is 400 wide, centred on the anchor

		if t.Color ~= nil then inst.Face:SetColor(t.Color); end
		local frame:number = working > 0 and FRAME_FILLED or FRAME_EMPTY;
		inst.Face:SetTextureOffsetVal(0, frame);
		inst.Rim:SetTextureOffsetVal(0, frame);
		inst.Button:SetAlpha(slots == 0 and 0.35 or 1);
		inst.Lock:SetHide(locked <= 0);

		local name:string = Locale.Lookup(t.Name or t.Type);
		local tip:string = name .. "[NEWLINE]" .. PerSpecialistText(pCity, t) .. " per citizen[NEWLINE]"
			.. "Working " .. working .. " of " .. slots .. " slots" .. (locked > 0 and (", " .. locked .. " locked") or "");
		if slots > 0 then tip = tip .. "[NEWLINE][NEWLINE]Left-click: lock one more citizen here[NEWLINE]Right-click: remove a lock"; end
		inst.Button:SetToolTipString(tip);

		inst.Button:RegisterCallback(Mouse.eLClick, function()
			local s:number, _, l:number = Values(pCity, t);
			if l < s then Request(pCity, t, l + 1); end
		end);
		inst.Button:RegisterCallback(Mouse.eRClick, function()
			local _, _, l:number = Values(pCity, t);
			if l > 0 then Request(pCity, t, l - 1); end
		end);
	end
	m_sLast = table.concat(snapshot, ",");
end

local function Hide()
	m_pCity = nil;
	m_SlotIM:ResetInstances();
	Controls.Anchor:SetHide(true);
	ContextPtr:ClearUpdate();
end

local function OnUpdate(delta:number)
	m_fSince = m_fSince + delta;
	if m_fSince < POLL_SECONDS then return; end
	m_fSince = 0;
	if m_pCity == nil then return; end
	local snapshot:table = {};
	for _, t in ipairs(m_kTypes) do
		local slots:number, working:number, locked:number = Values(m_pCity, t);
		table.insert(snapshot, slots .. "/" .. working .. "/" .. locked);
	end
	-- Do NOT raise engine events from here (Events.CityWorkerChanged.Call inside this update hung the game, 2026-10-30).
	if table.concat(snapshot, ",") ~= m_sLast then Refresh(); end
end

local function Show()
	if not m_bEnabled or not UILens.IsLayerOn(m_CitizenManagement) then Hide(); return; end
	local pCity:table = UI.GetHeadSelectedCity();
	if pCity == nil or pCity:GetOwner() ~= Game.GetLocalPlayer() then Hide(); return; end
	m_pCity = pCity;
	local plot:table = Map.GetPlot(pCity:GetX(), pCity:GetY());
	local worldX:number, worldY:number = UI.GridToWorld(plot:GetIndex());
	Controls.Anchor:SetWorldPositionVal(worldX, worldY, 20);
	Controls.Anchor:SetHide(false);
	Refresh();
	m_fSince = 0;
	ContextPtr:SetUpdate(OnUpdate);
end

-- ===========================================================================
local function OnLensLayerOn(layerNum:number)	if layerNum == m_CitizenManagement then Show(); end end
local function OnLensLayerOff(layerNum:number)	if layerNum == m_CitizenManagement then Hide(); end end
local function OnCityChanged()					if UILens.IsLayerOn(m_CitizenManagement) then Show(); end end

local function OnShutdown()
	Hide();
	m_SlotIM:DestroyInstances();
end

local function Initialize()
	ReadSetup();
	ContextPtr:SetShutdown(OnShutdown);
	if not m_bEnabled then return; end
	ContextPtr:SetHide(false);	-- an added InGame context starts hidden
	Events.LensLayerOn.Add(OnLensLayerOn);
	Events.LensLayerOff.Add(OnLensLayerOff);
	Events.CitySelectionChanged.Add(OnCityChanged);
	Events.CityWorkerChanged.Add(OnCityChanged);
	Events.CityFocusChanged.Add(OnCityChanged);
end
Initialize();
