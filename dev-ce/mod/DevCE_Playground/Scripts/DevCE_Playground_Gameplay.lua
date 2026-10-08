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

-- Dev CE: GameEras:SetCurrentEra(era). The engine function has no range check, so the target is clamped to the eras that exist here.
local function Era(owner, delta)
	local eras = Game.GetEras();
	if eras.SetCurrentEra == nil then NeedDevCE(owner, "Era"); return; end
	local count = 0;
	for _ in GameInfo.Eras() do count = count + 1; end
	local cur = nil;
	pcall(function() cur = eras:GetCurrentEra(); end);
	if cur == nil then Report(owner, "Era: cannot read the current era in the gameplay VM."); return; end
	local target = cur + delta;
	if target < 0 or target > count - 1 then Report(owner, "Era: " .. tostring(target) .. " is outside 0.." .. tostring(count - 1) .. ", nothing done."); return; end
	local ok, err = pcall(function() eras:SetCurrentEra(target); end);
	local after = "?";
	pcall(function() after = eras:GetCurrentEra(); end);
	Report(owner, "Era " .. (delta > 0 and "+1" or "-1") .. ": call " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; era " .. tostring(cur) .. " -> " .. tostring(after) .. " (see the Era line)");
end

GameEvents.DevCEPg_EraPlus.Add(function(owner) Era(owner, 1); end);
GameEvents.DevCEPg_EraMinus.Add(function(owner) Era(owner, -1); end);

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

-- ---------------------------------------------------------------- Sets B-I samples (Dev CE)
-- First other alive player for pair-wise methods (influence tokens, alliance
-- points). Falls back to self when alone: the stored value is still a valid
-- int pair, pcall-guarded, reversible with the paired button.
local function OtherPlayer(owner)
	local found = nil;
	pcall(function()
		for i = 0, 63 do
			if i ~= owner then
				local p = Players[i];
				if p ~= nil and p:IsAlive() then found = i; break; end
			end
		end
	end);
	if found == nil then found = owner; end
	return found;
end

-- B influence (Dev CE: PlayerInfluence:ChangeTokensReceived / GetTotalTokensReceived).
-- Prerequisite: none, any player works; counter-party shown in the report.
local function InfluenceTokens(owner, delta)
	local inf = nil;
	pcall(function() inf = Players[owner]:GetInfluence(); end);
	if inf == nil or inf.ChangeTokensReceived == nil or inf.GetTotalTokensReceived == nil then NeedDevCE(owner, "Influence tokens"); return; end
	local other = OtherPlayer(owner);
	local function Read() local v = "?"; pcall(function() v = inf:GetTotalTokensReceived(); end); return v; end
	if delta == 0 then Report(owner, "Influence tokens: total received " .. tostring(Read()) .. " (counter-party would be player " .. tostring(other) .. ")"); return; end
	local before = Read();
	local ok, err = pcall(function() inf:ChangeTokensReceived(other, delta); end);
	Report(owner, "Influence tokens " .. (delta > 0 and "+" or "") .. delta .. " (vs player " .. tostring(other) .. "): " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; total " .. tostring(before) .. " -> " .. tostring(Read()));
end

GameEvents.DevCEPg_BTokensRead.Add(function(owner) InfluenceTokens(owner, 0); end);
GameEvents.DevCEPg_BTokensPlus.Add(function(owner) InfluenceTokens(owner, 1); end);
GameEvents.DevCEPg_BTokensMinus.Add(function(owner) InfluenceTokens(owner, -1); end);

-- C diplomacy (Dev CE: TeamDiplomacy:GetNumMajorsMet / ChangeAlliancePointsWithPlayer).
-- Prerequisite: another civ in the game for the +/- pair (else counter-party is self).
local function AlliancePoints(owner, delta)
	local dip = nil;
	pcall(function() dip = Players[owner]:GetDiplomacy(); end);
	if dip == nil or dip.ChangeAlliancePointsWithPlayer == nil or dip.GetAlliancePointsWithPlayer == nil then NeedDevCE(owner, "Alliance points"); return; end
	local other = OtherPlayer(owner);
	local function Read() local v = "?"; pcall(function() v = dip:GetAlliancePointsWithPlayer(other); end); return v; end
	local before = Read();
	local ok, err = pcall(function() dip:ChangeAlliancePointsWithPlayer(other, delta); end);
	Report(owner, "Alliance points " .. (delta > 0 and "+" or "") .. delta .. " (vs player " .. tostring(other) .. "): " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; " .. tostring(before) .. " -> " .. tostring(Read()));
end

GameEvents.DevCEPg_CMetRead.Add(function(owner)
	local dip = nil;
	pcall(function() dip = Players[owner]:GetDiplomacy(); end);
	if dip == nil or dip.GetNumMajorsMet == nil then NeedDevCE(owner, "Majors met"); return; end
	local v = "?";
	pcall(function() v = dip:GetNumMajorsMet(); end);
	Report(owner, "Majors met: " .. tostring(v) .. " (meet more civs to test pair-wise buttons)");
end);
GameEvents.DevCEPg_CAllyPlus.Add(function(owner) AlliancePoints(owner, 1); end);
GameEvents.DevCEPg_CAllyMinus.Add(function(owner) AlliancePoints(owner, -1); end);

-- D culture/religion: read-only (Dev CE: PlayerCulture getters). No grant buttons by design.
-- Prerequisite: none.
local function CultureRead(owner, what, fn)
	local cult = nil;
	pcall(function() cult = Players[owner]:GetCulture(); end);
	if cult == nil then NeedDevCE(owner, what); return; end
	local v = "?";
	local ok, err = pcall(function() v = fn(cult); end);
	if not ok then NeedDevCE(owner, what .. " (" .. tostring(err) .. ")"); return; end
	Report(owner, what .. ": " .. tostring(v));
end

GameEvents.DevCEPg_DTourists.Add(function(owner) CultureRead(owner, "Tourists to you", function(c) return c:GetTouristsTo(); end); end);
GameEvents.DevCEPg_DParksMod.Add(function(owner) CultureRead(owner, "Tourism national-parks modifier", function(c) return c:GetTourismNationalParksModifier(); end); end);
GameEvents.DevCEPg_DCivics.Add(function(owner) CultureRead(owner, "Civics completed", function(c) return c:GetNumCivicsCompleted(false); end); end);

-- E city (Dev CE: City:ChangeFlatYieldBonusForDomestic on the capital; readout via the build queue).
-- Prerequisite: you own a capital city.
local function CapitalProd(owner, delta)
	local city = nil;
	pcall(function() city = Players[owner]:GetCities():GetCapitalCity(); end);
	if city == nil then Report(owner, "Capital production: no capital."); return; end
	if city.ChangeFlatYieldBonusForDomestic == nil then NeedDevCE(owner, "Capital production"); return; end
	local function Read() local v = "?"; pcall(function() v = city:GetYield(YieldTypes.PRODUCTION); end); return v; end
	local before = Read();
	local ok, err = pcall(function() city:ChangeFlatYieldBonusForDomestic(YieldTypes.PRODUCTION, delta); end);
	Report(owner, "Capital production flat " .. (delta > 0 and "+" or "") .. delta .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; city production yield " .. tostring(before) .. " -> " .. tostring(Read()) .. " (confirm on the city panel)");
end

GameEvents.DevCEPg_EBuildProg.Add(function(owner)
	local city = nil;
	pcall(function() city = Players[owner]:GetCities():GetCapitalCity(); end);
	if city == nil then Report(owner, "Capital build progress: no capital."); return; end
	local v, prog = "?", "?";
	pcall(function()
		local bq = city:GetBuildQueue();
		if bq ~= nil and bq.GetCurrentBuildProgress ~= nil then prog = bq:GetCurrentBuildProgress(); end
		if city.GetYield ~= nil then v = city:GetYield(YieldTypes.PRODUCTION); end
	end);
	if prog == "?" then NeedDevCE(owner, "Capital build progress"); return; end
	Report(owner, "Capital build progress: " .. tostring(prog) .. " (production yield " .. tostring(v) .. ")");
end);
GameEvents.DevCEPg_EProdPlus.Add(function(owner) CapitalProd(owner, 1); end);
GameEvents.DevCEPg_EProdMinus.Add(function(owner) CapitalProd(owner, -1); end);

-- F player treasury (Dev CE: PlayerTreasury:SetGoldRateChange, absolute SET).
-- One-way by nature: the +5 button sets an absolute bonus, the restore button sets 0.
-- Prerequisite: none; use a disposable save, it changes your gold income.
local function GoldRate(owner, value, label)
	local treas = nil;
	pcall(function() treas = Players[owner]:GetTreasury(); end);
	if treas == nil or treas.SetGoldRateChange == nil then NeedDevCE(owner, "Gold rate"); return; end
	local function Read() local v = "?"; pcall(function() v = treas:GetGoldYield(); end); return v; end
	local before = Read();
	local ok, err = pcall(function() treas:SetGoldRateChange(value); end);
	Report(owner, "Gold rate " .. label .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; gold yield/turn " .. tostring(before) .. " -> " .. tostring(Read()) .. " (disposable save)");
end

GameEvents.DevCEPg_FGoldSet.Add(function(owner) GoldRate(owner, 5, "SET +5"); end);
GameEvents.DevCEPg_FGoldRestore.Add(function(owner) GoldRate(owner, 0, "restore 0"); end);

-- G world era score (Dev CE: GameEras:GetPlayerCurrentScore / ChangeEraScore).
-- ChangeEraScore takes an EraScoreTypes value with no proven enum yet, so 0 is
-- passed (selftest used arbitrary ints the same way); the before/after score
-- readout confirms the effect. Prerequisite: none; disposable save.
local function EraScore(owner, delta)
	local eras = Game.GetEras();
	if eras == nil or eras.GetPlayerCurrentScore == nil or eras.ChangeEraScore == nil then NeedDevCE(owner, "Era score"); return; end
	local function Read() local v = "?"; pcall(function() v = eras:GetPlayerCurrentScore(owner); end); return v; end
	if delta == 0 then Report(owner, "Era score: current " .. tostring(Read())); return; end
	local before = Read();
	local ok, err = pcall(function() eras:ChangeEraScore(owner, delta, 0); end);
	Report(owner, "Era score " .. (delta > 0 and "+" or "") .. delta .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; " .. tostring(before) .. " -> " .. tostring(Read()));
end

GameEvents.DevCEPg_GScoreRead.Add(function(owner) EraScore(owner, 0); end);
GameEvents.DevCEPg_GScorePlus.Add(function(owner) EraScore(owner, 1); end);
GameEvents.DevCEPg_GScoreMinus.Add(function(owner) EraScore(owner, -1); end);

-- H unit (Dev CE: Unit:ChangeParkCharges reversible; UnitExperience:SetLevelAndExperience absolute).
-- Prerequisite: select a disposable unit first (the carrier); park charges need a unit that has them.
local function ParkCharges(owner, unitID, delta)
	local u = GetUnit(owner, unitID);
	if u == nil then Report(owner, "Park charges: selected unit not found."); return; end
	if u.ChangeParkCharges == nil then NeedDevCE(owner, "Park charges"); return; end
	local function Read() local v = "?"; pcall(function() v = u:GetParkCharges(); end); return v; end
	local before = Read();
	local ok, err = pcall(function() u:ChangeParkCharges(delta); end);
	Report(owner, "Park charges " .. (delta > 0 and "+" or "") .. delta .. ": " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; " .. tostring(before) .. " -> " .. tostring(Read()) .. " (confirm on the unit panel)");
end

GameEvents.DevCEPg_HParkPlus.Add(function(owner, unitID) ParkCharges(owner, unitID, 1); end);
GameEvents.DevCEPg_HParkMinus.Add(function(owner, unitID) ParkCharges(owner, unitID, -1); end);
GameEvents.DevCEPg_HLevelPlus.Add(function(owner, unitID)
	local u = GetUnit(owner, unitID);
	if u == nil then Report(owner, "XP level: selected unit not found."); return; end
	local exp = nil;
	pcall(function() exp = u:GetExperience(); end);
	if exp == nil or exp.SetLevelAndExperience == nil then NeedDevCE(owner, "XP level"); return; end
	local before, bexp = "?", "?";
	pcall(function() before = exp:GetLevel(); end);
	pcall(function() bexp = exp:GetExperiencePoints(); end);
	if before == "?" then Report(owner, "XP level: cannot read the unit level."); return; end
	local ok, err = pcall(function() exp:SetLevelAndExperience(before + 1, 0); end);
	local after = "?";
	pcall(function() after = exp:GetLevel(); end);
	Report(owner, "XP level +1 (one-way, disposable unit): " .. (ok and "ok" or ("failed: " .. tostring(err))) .. "; level " .. tostring(before) .. " (xp " .. tostring(bexp) .. ") -> " .. tostring(after));
end);

-- R41 manual probe: Deal + DealItem only make sense mid-negotiation, so this
-- button runs on demand instead of at turn start. Self-contained (no cross-file
-- globals: gameplay script contexts do not reliably share _G here).
local R41_DEAL_METHODS = { "RemoveExpiredItems", "DoTurn", "IsExpired", "DevOracle_HasUnacceptableItems" };
local R41_DEALITEM_METHODS = { "GetParentType" };
local function R41DealProbe(owner)
	local parts = {};
	local deal = nil;
	pcall(function()
		if DealManager == nil then return end
		-- Working deal needs both parties (vanilla UI passes from/to IDs);
		-- brute-force read-only over majors since the other party is unknown.
		for other = 0, 62 do
			if owner ~= nil and other ~= owner then
				local cands = {
					function() return DealManager.GetWorkingDeal(owner, other) end,
					function() return DealManager.GetWorkingDeal(other, owner) end,
					function() return DealManager.GetWorkingDeal(0, owner, other) end,
					function() return DealManager.GetWorkingDeal(1, owner, other) end,
					function() return DealManager.GetWorkingDeal(0, other, owner) end,
					function() return DealManager.GetWorkingDeal(1, other, owner) end,
				};
				for _, f in ipairs(cands) do
					pcall(function() deal = f() end);
					if deal ~= nil then return end
				end
			end
		end
	end);
	if deal == nil then
		return "IDeal NO-INSTANCE (nothing under negotiation) | IDealItem NO-INSTANCE (no deal)";
	end
	local function have(inst)
		local set = {};
		pcall(function()
			local mt = getmetatable(inst);
			local idx = mt and mt.__index;
			if type(idx) == "table" then for k, v in pairs(idx) do set[tostring(k)] = true end end
		end);
		return set;
	end
	local dh = have(deal);
	local dp, dm = 0, {};
	for _, m in ipairs(R41_DEAL_METHODS) do
		if dh[m] then dp = dp + 1 else dm[#dm + 1] = m end
	end
	parts[#parts + 1] = "IDeal " .. tostring(dp) .. " present, " .. tostring(#dm) .. " missing"
		.. (#dm > 0 and (" (" .. table.concat(dm, ",") .. ")") or "");
	local item = nil;
	pcall(function()
		if deal.Items ~= nil then for it in deal:Items() do item = it; break; end end
	end);
	if item == nil then
		parts[#parts + 1] = "IDealItem NO-INSTANCE (deal has no items)";
	else
		local ih = have(item);
		parts[#parts + 1] = "IDealItem " .. (ih["GetParentType"] and "1 present, 0 missing" or "0 present, 1 missing (GetParentType)");
	end
	return table.concat(parts, " | ");
end
GameEvents.DevCEPg_R41Deal.Add(function(owner)
	local ok, msg = pcall(R41DealProbe, owner);
	Report(owner, "R41 deal (press while negotiating): " .. (ok and tostring(msg) or ("failed: " .. tostring(msg))));
end);

print("[DevCEPg][GP] ready.");
