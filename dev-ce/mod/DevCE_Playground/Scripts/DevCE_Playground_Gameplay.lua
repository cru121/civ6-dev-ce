-- =====================================================================
--  DevCE_Playground_Gameplay.lua   (runs in the GAMEPLAY Lua context)
--
--  Handlers for the Playground panel's buttons. The UI sends an EXECUTE_SCRIPT unit command named like the
--  GameEvents entry below; the engine calls the handler with (owner, unit id, parameters).
--  Dev CE methods are checked for before use: without the Dev CE GameCore mod those buttons only report that.
--  Every handler reports before/after values through two player properties (DEVCEPG_SEQ, DEVCEPG_MSG) and print().
-- =====================================================================

print("[DevCEPg][GP] loading...");

local seq = 0;

local function Report(owner, msg)
	seq = seq + 1;
	print("[DevCEPg][GP] " .. msg);
	pcall(function()
		Players[owner]:SetProperty("DEVCEPG_MSG", msg);
		Players[owner]:SetProperty("DEVCEPG_SEQ", seq);
	end);
end

local function NeedDevCE(owner, what)
	Report(owner, what .. ": the Dev CE GameCore mod is not loaded (enable 'Dev CE (experimental)' or 'Dev CE test build').");
end

local function GetUnit(owner, unitID)
	local u = nil;
	pcall(function() u = Players[owner]:GetUnits():FindID(unitID); end);
	return u;
end

-- ---------------------------------------------------------------- eras (Dev CE: GameEras:SetGoldenAge / SetDarkAge)
local function SetAge(owner, golden, state)
	local eras = Game.GetEras();
	local setter = golden and eras.SetGoldenAge or eras.SetDarkAge;
	local name = (golden and "Golden" or "Dark") .. " Age " .. (state and "ON" or "OFF");
	if setter == nil then NeedDevCE(owner, name); return; end
	-- the Has...Age getters exist on the UI side only; the panel shows their value ("Ages" line), so no read-back here
	local ok, err = pcall(function() setter(eras, owner, state); end);
	Report(owner, name .. ": call " .. (ok and "ok" or ("failed: " .. tostring(err))) .. " (see the Ages line)");
end

GameEvents.DevCEPg_GoldenOn.Add(function(owner) SetAge(owner, true, true); end);
GameEvents.DevCEPg_GoldenOff.Add(function(owner) SetAge(owner, true, false); end);
GameEvents.DevCEPg_DarkOn.Add(function(owner) SetAge(owner, false, true); end);
GameEvents.DevCEPg_DarkOff.Add(function(owner) SetAge(owner, false, false); end);

-- ---------------------------------------------------------------- spawn a named great person (vanilla: GameGreatPeople:GrantPerson, as the FreeGP mod does)
GameEvents.DevCEPg_SpawnJames.Add(function(owner)
	local row = GameInfo.GreatPersonIndividuals["GREAT_PERSON_INDIVIDUAL_JAMES_OF_ST_GEORGE"];
	if row == nil then Report(owner, "James of St. George is not in this game's database."); return; end
	local classIdx, eraIdx = 0, 0;
	local c = row.GreatPersonClassType and GameInfo.GreatPersonClasses[row.GreatPersonClassType];
	if c ~= nil then classIdx = c.Index; end
	local e = row.EraType and GameInfo.Eras[row.EraType];
	if e ~= nil then eraIdx = e.Index; end
	local before = Players[owner]:GetUnits():GetCount();
	local ok, err = pcall(function() Game.GetGreatPeople():GrantPerson(row.Index, classIdx, eraIdx, 0, owner, false); end);
	local after = Players[owner]:GetUnits():GetCount();
	Report(owner, "Spawn James of St. George: " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; units " .. tostring(before) .. " -> " .. tostring(after) .. " (he appears in your capital)");
end);

-- ---------------------------------------------------------------- action charges of the selected unit
-- vanilla: Unit:ChangeActionCharges (exists in the base game).  Dev CE: UnitGreatPerson:ChangeActionCharges, the engine function the
-- modifier effect EFFECT_ADJUST_UNIT_GREAT_PERSON_CHARGES uses. They may or may not behave the same: the buttons let you compare.
local function Charges(owner, unitID, delta, devce)
	local u = GetUnit(owner, unitID);
	if u == nil then Report(owner, "Charges: selected unit not found."); return; end
	local gp = nil;
	pcall(function() gp = u:GetGreatPerson(); end);
	local function Read()
		local a, b = "?", "?";
		pcall(function() a = u:GetActionCharges(); end);
		pcall(function() if gp ~= nil then b = gp:GetActionCharges(); end end);
		return "unit " .. tostring(a) .. " / great-person " .. tostring(b);
	end
	local label = (devce and "Dev CE " or "vanilla ") .. (delta > 0 and "+1" or "-1");
	if devce then
		if gp == nil or gp.ChangeActionCharges == nil then NeedDevCE(owner, "Charges " .. label); return; end
	end
	local before = Read();
	local ok, err = pcall(function()
		if devce then gp:ChangeActionCharges(delta); else u:ChangeActionCharges(delta); end
	end);
	Report(owner, "Charges " .. label .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; " .. before .. "  ->  " .. Read());
end

GameEvents.DevCEPg_VChargePlus.Add(function(owner, unitID) Charges(owner, unitID, 1, false); end);
GameEvents.DevCEPg_VChargeMinus.Add(function(owner, unitID) Charges(owner, unitID, -1, false); end);
GameEvents.DevCEPg_DChargePlus.Add(function(owner, unitID) Charges(owner, unitID, 1, true); end);
GameEvents.DevCEPg_DChargeMinus.Add(function(owner, unitID) Charges(owner, unitID, -1, true); end);

-- ---------------------------------------------------------------- capital food (Dev CE: City:ChangeYieldChange, flat yield bonus)
local function Food(owner, delta)
	local city = nil;
	pcall(function() city = Players[owner]:GetCities():GetCapitalCity(); end);
	if city == nil then Report(owner, "Capital food: no capital."); return; end
	if city.ChangeYieldChange == nil then NeedDevCE(owner, "Capital food"); return; end
	local function Read() local v = "?"; pcall(function() v = city:GetGrowth():GetFoodSurplus(); end); return v; end
	local before = Read();
	local ok, err = pcall(function() city:ChangeYieldChange(YieldTypes.FOOD, delta); end);
	Report(owner, "Capital food " .. (delta > 0 and "+" or "") .. delta .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; food surplus " .. tostring(before) .. " -> " .. tostring(Read()));
end

GameEvents.DevCEPg_FoodPlus.Add(function(owner) Food(owner, 2); end);
GameEvents.DevCEPg_FoodMinus.Add(function(owner) Food(owner, -2); end);

-- ---------------------------------------------------------------- trade route capacity (Dev CE: PlayerTrade:ChangeOutgoingRouteCapacity)
local function Routes(owner, delta)
	local trade = nil;
	pcall(function() trade = Players[owner]:GetTrade(); end);
	if trade == nil or trade.ChangeOutgoingRouteCapacity == nil then NeedDevCE(owner, "Trade routes"); return; end
	local function Read() local v = "?"; pcall(function() v = trade:GetOutgoingRouteCapacity(); end); return v; end
	local before = Read();
	local ok, err = pcall(function() trade:ChangeOutgoingRouteCapacity(delta); end);
	Report(owner, "Trade routes " .. (delta > 0 and "+" or "") .. delta .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; capacity " .. tostring(before) .. " -> " .. tostring(Read()));
end

GameEvents.DevCEPg_RoutesPlus.Add(function(owner) Routes(owner, 1); end);
GameEvents.DevCEPg_RoutesMinus.Add(function(owner) Routes(owner, -1); end);

print("[DevCEPg][GP] ready.");
