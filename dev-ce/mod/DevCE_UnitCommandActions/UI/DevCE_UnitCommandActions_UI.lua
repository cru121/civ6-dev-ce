-- =====================================================================
--  DevCE_UnitCommandActions_UI.lua   (InGame UI context)
--
--  Hovering a Dev CE unit command's button in the unit panel highlights the plots where the selected unit's owner could use it
--  (the command's template great person, UnitCommandActions.GetActivationPlots), on the great people layer, as a great person's
--  activation plots are shown. Leaving the button clears it.
--
--  The unit panel rebuilds its buttons often and re-registers their mouse-enter callback (a sound) each time, so the buttons are
--  re-scanned a few times per second. A command button is recognised by its Void2 (the command hash the unit panel stores there).
-- =====================================================================

local LAYER = UILens.CreateLensLayerHash("Hex_Coloring_Great_People");
local SCAN_INTERVAL = 0.25;   -- seconds

local m_stacks = nil;         -- the unit panel's action stacks
local m_ours = {};            -- command hash (unsigned) -> true
local m_shownHash = nil;      -- command currently highlighted
local m_shownButton = nil;    -- its button
local m_sinceScan = 0;

local function Unsigned(h)
	if h == nil then return nil; end
	if h < 0 then return h + 4294967296; end
	return h;
end

local function Available()
	return UnitCommandActions ~= nil and UnitCommandActions.GetActivationPlots ~= nil;
end

local function RefreshOurCommands()
	m_ours = {};
	if not Available() then return; end
	for _, h in ipairs(UnitCommandActions.GetCommandHashes()) do m_ours[Unsigned(h)] = true; end
end

local function FindStacks()
	for _, base in ipairs({ "/InGame/UnitPanel", "/InGame/HUD/UnitPanel" }) do
		local standard = ContextPtr:LookUpControl(base .. "/StandardActionsStack");
		if standard ~= nil then
			local stacks = { standard };
			local secondary = ContextPtr:LookUpControl(base .. "/SecondaryActionsStack");
			if secondary ~= nil then table.insert(stacks, secondary); end
			return stacks;
		end
	end
	return nil;
end

local function HidePlots()
	if m_shownHash == nil then return; end
	m_shownHash = nil;
	m_shownButton = nil;
	UILens.ClearLayerHexes(LAYER);
	if UILens.IsLayerOn(LAYER) then UILens.ToggleLayerOff(LAYER); end
end

local function ShowPlots(hash, button)
	local unit = UI.GetHeadSelectedUnit();
	if unit == nil or UI.IsGameCoreBusy() then return; end
	local player = unit:GetOwner();
	if player ~= Game.GetLocalPlayer() then return; end
	local ok, plots = pcall(UnitCommandActions.GetActivationPlots, hash, player);
	if not ok or plots == nil then return; end
	local hexes = {};
	for _, plotIndex in ipairs(plots) do table.insert(hexes, { "Great_People", plotIndex }); end
	UILens.ClearLayerHexes(LAYER);
	UILens.SetLayerHexesArea(LAYER, player, {}, hexes);
	UILens.ToggleLayerOn(LAYER);
	m_shownHash = hash;
	m_shownButton = button;
end

-- Adds our enter/exit callbacks to the buttons of our commands (the unit panel replaced them if it rebuilt the button).
local function WireButtons()
	if m_stacks == nil then m_stacks = FindStacks(); end
	if m_stacks == nil then return; end
	for _, stack in ipairs(m_stacks) do
		for _, button in ipairs(stack:GetChildren()) do
			local hash = Unsigned(button:GetVoid2());
			if hash ~= nil and m_ours[hash] and not button:IsHidden() then
				button:RegisterCallback(Mouse.eMouseEnter, function()
					UI.PlaySound("Main_Menu_Mouse_Over");   -- what the unit panel's own callback did
					ShowPlots(hash, button);
				end);
				button:RegisterCallback(Mouse.eMouseExit, function()
					if m_shownButton == button then HidePlots(); end
				end);
			end
		end
	end
end

local function OnUpdate(deltaTime)
	m_sinceScan = m_sinceScan + deltaTime;
	if m_sinceScan < SCAN_INTERVAL then return; end
	m_sinceScan = 0;
	if next(m_ours) == nil then RefreshOurCommands(); end   -- the gameplay script defines them after the UI loads
	if next(m_ours) == nil then return; end
	WireButtons();
	-- The button went away under the cursor (panel hidden, rebuilt for another action): no exit event comes.
	if m_shownButton ~= nil and (m_shownButton:IsHidden() or Unsigned(m_shownButton:GetVoid2()) ~= m_shownHash) then HidePlots(); end
end

local function OnSelectionChanged()
	HidePlots();
end

local function Initialize()
	if not Available() then
		print("[DevCEUnitCommandActions UI] Dev CE UnitCommandActions UI functions not found: no hover highlight");
		return;
	end
	Events.UnitSelectionChanged.Add(OnSelectionChanged);
	Events.LoadGameViewStateDone.Add(RefreshOurCommands);
	ContextPtr:SetUpdate(OnUpdate);
	ContextPtr:SetShutdown(HidePlots);
end
Initialize();
