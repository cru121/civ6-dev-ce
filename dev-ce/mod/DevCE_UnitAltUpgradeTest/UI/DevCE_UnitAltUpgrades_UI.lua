-- =====================================================================
--  DevCE_UnitAltUpgrades_UI.lua   (InGame UI context)
--
--  One extra Upgrade button in the unit panel per alternative upgrade (DevCE_UnitAltUpgrades), next to the game's own Upgrade
--  button. The buttons use the game's UPGRADE command with the choice as a parameter, so the Dev CE GameCore does the checks,
--  the cost and the upgrade:
--    UnitManager.CanStartCommand(unit, UPGRADE, { [UnitUpgradeRules.PARAM_CHOICE] = choiceIndex + 1 }, true)
--    UnitManager.RequestCommand(unit, UPGRADE, same parameters)
--  A button shows when the game's Upgrade button would (the upgrade is unlocked and the unit can still move) and is disabled,
--  with the reasons in red, when the upgrade cannot be done right now (gold, territory, level, limits, buildings).
--
--  It does not replace UnitPanel.lua (other mods do): the buttons are made in this context and moved into the unit panel's
--  StandardActionsStack once the game view is loaded, and refreshed on the selection and unit events.
-- =====================================================================

local MAX_BUTTONS = 8;
local MIN_UNIT_PANEL_WIDTH = 340;    -- UnitPanel.lua constants, for the panel width after our buttons changed
local UNIT_PANEL_PADDING = 18;
local LOG = "[DevCEUnitAltUpgradesUI] ";

local m_panel = nil;                 -- { Base, Actions, Standard } controls of the unit panel
local m_buttons = {};                -- our button instances (Root grid, UnitActionButton, UnitActionIcon)
local m_choices = {};                -- unit type -> { choice unit index, ... } (DevCE_UnitAltUpgrades, unique units included)
local m_selectedPlayer, m_selectedUnit = -1, -1;

local function Available()
	return UnitUpgradeRules ~= nil and UnitUpgradeRules.PARAM_CHOICE ~= nil and GameInfo.DevCE_UnitAltUpgrades ~= nil;
end

-- ---------------------------------------------------------------------
-- The choices of each unit type, as the gameplay script gives them to Dev CE: a unique unit gets the rows of the unit it
-- replaces unless it has rows of its own (the GameCore turns an UpgradeUnit into the player's own unique version).
-- ---------------------------------------------------------------------
local function BuildChoices()
	m_choices = {};
	local rows = {};
	for row in GameInfo.DevCE_UnitAltUpgrades() do
		local from, to = GameInfo.Units[row.Unit], GameInfo.Units[row.UpgradeUnit];
		if from ~= nil and to ~= nil then
			rows[row.Unit] = rows[row.Unit] or {};
			table.insert(rows[row.Unit], to.Index);
		end
	end
	for unitType, list in pairs(rows) do
		m_choices[GameInfo.Units[unitType].Index] = list;
		for r in GameInfo.UnitReplaces() do
			if r.ReplacesUnitType == unitType and rows[r.CivUniqueUnitType] == nil and GameInfo.Units[r.CivUniqueUnitType] ~= nil then
				m_choices[GameInfo.Units[r.CivUniqueUnitType].Index] = list;
			end
		end
	end
end

-- The unit panel's own ResizeUnitPanelToFitActionButtons, after our buttons changed the row.
local function ResizePanel()
	if m_panel == nil then return; end
	m_panel.Standard:CalculateSize();
	m_panel.Actions:CalculateSize();
	local width = m_panel.Actions:GetSizeX();
	m_panel.Base:SetSizeX(width > MIN_UNIT_PANEL_WIDTH and (width + UNIT_PANEL_PADDING) or MIN_UNIT_PANEL_WIDTH);
end

local function HideButtons()
	for _, inst in ipairs(m_buttons) do inst.Root:SetHide(true); end
end

-- ---------------------------------------------------------------------
-- Tooltip as the game's Upgrade button: "Upgrade to X (cost)", the strategic resource, then the failure reasons in red.
-- ---------------------------------------------------------------------
local function Tooltip(targetIndex, tResults, canStart)
	local target = GameInfo.Units[targetIndex];
	local cost = tResults[UnitUpgradeRules.RESULT_COST];
	local tip = cost ~= nil and Locale.Lookup("LOC_UNITOPERATION_UPGRADE_INFO", target.Name, cost) or Locale.Lookup(target.Name);
	local resourceCost = tResults[UnitUpgradeRules.RESULT_RESOURCE_COST];
	local resource = target.StrategicResource ~= nil and GameInfo.Resources[target.StrategicResource] or nil;
	if resource ~= nil and resourceCost ~= nil and resourceCost > 0 and GameInfo.Units_XP2 ~= nil then
		tip = tip .. "[NEWLINE]" .. Locale.Lookup("LOC_UNITOPERATION_UPGRADE_RESOURCE_INFO", resourceCost, "[ICON_" .. resource.ResourceType .. "]", resource.Name);
	end
	if not canStart and tResults[UnitOperationResults.FAILURE_REASONS] ~= nil then
		for _, reason in ipairs(tResults[UnitOperationResults.FAILURE_REASONS]) do
			tip = tip .. "[NEWLINE][COLOR:Red]" .. Locale.Lookup(reason) .. "[ENDCOLOR]";
		end
	end
	return tip;
end

local Refresh;

local function OnClick(unitOwner, unitID, choiceIndex)
	local unit = UnitManager.GetUnit(unitOwner, unitID);
	if unit == nil then return; end
	local params = { [UnitUpgradeRules.PARAM_CHOICE] = choiceIndex + 1 };
	if UnitManager.CanStartCommand(unit, UnitCommandTypes.UPGRADE, params) then
		UnitManager.RequestCommand(unit, UnitCommandTypes.UPGRADE, params);
	end
	HideButtons();
	ResizePanel();
end

-- ---------------------------------------------------------------------
-- Show the buttons for a unit (or none).
-- ---------------------------------------------------------------------
Refresh = function(playerID, unitID)
	if m_panel == nil then return; end
	HideButtons();
	local unit = (playerID ~= nil and playerID >= 0) and UnitManager.GetUnit(playerID, unitID) or nil;
	local choices = unit ~= nil and m_choices[unit:GetType()] or nil;
	if choices ~= nil and unit:GetOwner() == Game.GetLocalPlayer() then
		-- the game's own Upgrade button: an alternative that ends at the same unit is not shown twice
		local _, tDefault = UnitManager.CanStartCommand(unit, UnitCommandTypes.UPGRADE, false, true);
		local defaultTarget = tDefault ~= nil and tDefault[UnitCommandResults.UNIT_TYPE] or nil;

		local used, shown = 0, {};
		for _, choiceIndex in ipairs(choices) do
			local params = { [UnitUpgradeRules.PARAM_CHOICE] = choiceIndex + 1 };
			local canStart, tResults = UnitManager.CanStartCommand(unit, UnitCommandTypes.UPGRADE, params, true);
			local target = tResults ~= nil and tResults[UnitCommandResults.UNIT_TYPE] or nil;
			-- no target: not unlocked, or the unit cannot move (the game's own button is not shown then either)
			if target ~= nil and target ~= defaultTarget and not shown[target] and used < #m_buttons then
				shown[target] = true;
				used = used + 1;
				local inst = m_buttons[used];
				local x, y, sheet = IconManager:FindIconAtlas("ICON_" .. GameInfo.Units[target].UnitType, 38);
				if sheet ~= nil then inst.UnitActionIcon:SetTexture(x, y, sheet); else inst.UnitActionIcon:SetIcon("ICON_UNITCOMMAND_UPGRADE"); end
				inst.UnitActionButton:SetDisabled(not canStart);
				inst.Root:SetAlpha(canStart and 1 or 0.7);
				inst.UnitActionButton:SetToolTipString(Tooltip(target, tResults, canStart));
				local owner, id = unit:GetOwner(), unit:GetID();
				inst.UnitActionButton:RegisterCallback(Mouse.eLClick, function() OnClick(owner, id, choiceIndex); end);
				inst.UnitActionButton:RegisterCallback(Mouse.eMouseEnter, function() UI.PlaySound("Main_Menu_Mouse_Over"); end);
				inst.Root:SetHide(false);
			end
		end
	end
	ResizePanel();
end

local function RefreshSelected()
	local unit = UI.GetHeadSelectedUnit();
	if unit ~= nil then
		m_selectedPlayer, m_selectedUnit = unit:GetOwner(), unit:GetID();
	else
		m_selectedPlayer, m_selectedUnit = -1, -1;
	end
	Refresh(m_selectedPlayer, m_selectedUnit);
end

-- ---------------------------------------------------------------------
-- Events
-- ---------------------------------------------------------------------
local function OnUnitSelectionChanged(playerID, unitID, x, y, z, selected, editable)
	if selected then
		m_selectedPlayer, m_selectedUnit = playerID, unitID;
		Refresh(playerID, unitID);
	elseif playerID == m_selectedPlayer and unitID == m_selectedUnit then
		m_selectedPlayer, m_selectedUnit = -1, -1;
		HideButtons();
		ResizePanel();
	end
end

local function OnSelectedUnitEvent(playerID, unitID)
	if playerID == m_selectedPlayer and unitID == m_selectedUnit then Refresh(playerID, unitID); end
end

local function OnPlayerEvent(playerID)
	if playerID == Game.GetLocalPlayer() then RefreshSelected(); end
end

-- Once the game view is loaded the unit panel exists: move our buttons into its action row.
local function Setup()
	if m_panel ~= nil then return; end   -- once (the load event, or a hot reload)
	local base = "/InGame/UnitPanel";
	local standard = ContextPtr:LookUpControl(base .. "/StandardActionsStack");
	local actions = ContextPtr:LookUpControl(base .. "/ActionsStack");
	local container = ContextPtr:LookUpControl(base .. "/UnitPanelBaseContainer");
	if standard == nil or actions == nil or container == nil then
		print(LOG .. "unit panel controls not found: no extra Upgrade buttons");
		return;
	end
	m_panel = { Standard = standard, Actions = actions, Base = container };
	for i = 1, MAX_BUTTONS do
		local inst = {};
		ContextPtr:BuildInstanceForControl("AltUpgradeInstance", inst, Controls.Pool);
		inst.Root:ChangeParent(standard);
		inst.Root:SetHide(true);
		m_buttons[i] = inst;
	end
	RefreshSelected();
end

local function Initialize()
	if not Available() then
		print(LOG .. "Dev CE alternative upgrades are not available: no extra Upgrade buttons");
		return;
	end
	BuildChoices();
	local n = 0; for _ in pairs(m_choices) do n = n + 1; end
	print(LOG .. n .. " unit types with alternative upgrades");

	local function On(name, fn)
		if Events[name] ~= nil then Events[name].Add(fn); else print(LOG .. "no event " .. name); end
	end
	On("LoadGameViewStateDone", Setup);
	On("UnitSelectionChanged", OnUnitSelectionChanged);
	On("UnitOperationsCleared", OnSelectedUnitEvent);
	On("UnitMoveComplete", OnSelectedUnitEvent);
	On("UnitCommandStarted", OnSelectedUnitEvent);
	On("UnitPromoted", OnSelectedUnitEvent);
	On("UnitUpgraded", OnPlayerEvent);
	On("TreasuryChanged", OnPlayerEvent);
	On("PlayerTurnActivated", OnPlayerEvent);
	On("ResearchCompleted", OnPlayerEvent);

	-- loaded by a hot reload, after the game view was loaded already
	if ContextPtr:LookUpControl("/InGame/UnitPanel/StandardActionsStack") ~= nil then Setup(); end
end

Initialize();
