-- =====================================================================
--  DevCE_Reach41_Gameplay.lua   (runs in the GAMEPLAY Lua context)
--
--  Covers the 7 interfaces the DevCE selftest marks NOT REACHED because its
--  generator emits reach = nil for them (Deal 4, DealItem 1, District 21,
--  FreeCities 2, FalloutManager 6, AreaPortal 3, Territory 4 = 41 methods).
--  Hand-written reach paths (vanilla APIs only), all pcall-guarded: an
--  interface with no live instance in THIS save reports NO-INSTANCE with the
--  reason instead of failing. Runs once on the first turn start of the local
--  player (late saves: the next turn after load), results go to Lua.log
--  as [DevCEPg][R41] lines. Read-only: never calls mutators, never writes
--  game state. Part of playground-bi-samples, not of upstream DevCE_Test.
-- =====================================================================

print("[DevCEPg][R41] loading...");

local R41_DONE = false;

local EXPECT41 = {
	{ iface = "IDeal", methods = { "RemoveExpiredItems", "DoTurn", "IsExpired", "DevOracle_HasUnacceptableItems" } },
	{ iface = "IDealItem", methods = { "GetParentType" } },
	{ iface = "IDistrict", methods = { "SetSiegeStatus", "ChangeRemainingAttackCount", "ChangeExtraRegionalYield", "ChangeGreatPersonPointChange", "ChangeTourismAdjacencyYieldModifier", "SetComplete", "AddYieldAdjacencyBonusMirror", "CalculateTourismAdjacencyYieldModifier", "GetAirSlots", "GetAllCalculatedTourismAdjacencyYieldModifier", "GetAppealYield", "GetExtraRegionalYield", "GetFirstPillagableBuilding", "GetMilitaryDomain", "HasGarrisonedUnit", "HasMaxDamage", "HasWalls", "IsBesieged", "RemoveYieldAdjacencyBonusMirror", "DevOracle_GetOwner", "DevOracle_IsPillaged" } },
	{ iface = "IFreeCities", methods = { "SetAlive", "StartRetaliationBehaviorTree" } },
	{ iface = "IGameFalloutManager", methods = { "AddNuclearReactor", "DoTurn", "GetFalloutDamage", "GetReactorAge", "ResetReactorAge", "UpdateFallout" } },
	{ iface = "IMapAreaPortal", methods = { "AttachAreas", "AddSourceAt", "RemoveSourceAt" } },
	{ iface = "IMapTerritory", methods = { "SetIsSea", "ChangeFeatureCount", "ChangePlotCount", "SetIsLake" } },
};

local function R41Log(msg) print("[DevCEPg][R41] " .. msg); end

local function Methods(inst)
	local set = {};
	pcall(function()
		local mt = getmetatable(inst);
		local idx = mt and mt.__index;
		if type(idx) == "table" then for k, v in pairs(idx) do set[tostring(k)] = true end end
	end);
	return set;
end

-- Returns instance + path-note, or nil + reason. Vanilla APIs only.
local function Reach(iface, p, capital)
	if iface == "IDistrict" then
		if capital == nil then return nil, "no capital city" end
		local ok, ds = pcall(function() return capital:GetDistricts() end);
		if not ok or ds == nil then return nil, "capital:GetDistricts() failed" end
		local ok2, n = pcall(function() return ds:GetNumDistricts() end);
		if not ok2 or (n or 0) == 0 then return nil, "capital has 0 districts" end
		local d = nil;
		pcall(function() d = ds:GetDistrictByIndex(0) end);
		if d == nil then return nil, "GetDistrictByIndex(0) nil" end
		return d, "capital district #0 of " .. tostring(n);
	elseif iface == "IDeal" then
		local ok, dm = pcall(function() return DealManager end);
		if not ok or dm == nil then return nil, "no DealManager global" end
		local deal = nil;
		pcall(function() deal = dm.GetWorkingDeal() end);
		if deal == nil then return nil, "no working deal (nothing under negotiation)" end
		return deal, "working deal";
	elseif iface == "IDealItem" then
		local deal, why = Reach("IDeal", p, capital);
		if deal == nil then return nil, "no deal: " .. tostring(why) end
		local item = nil;
		pcall(function()
			if deal.Items ~= nil then for it in deal:Items() do item = it; break; end end
		end);
		if item == nil then return nil, "deal has no items" end
		return item, "first item of working deal";
	elseif iface == "IFreeCities" then
		local ok, pm = pcall(function() return PlayerManager end);
		if not ok or pm == nil then return nil, "no PlayerManager global" end
		local id = nil;
		pcall(function() id = pm.GetFreeCitiesPlayerID() end);
		if id == nil or id < 0 then return nil, "no free-cities player in this game" end
		local fp = nil;
		pcall(function() fp = Players[id] end);
		if fp == nil then return nil, "free-cities player id " .. tostring(id) .. " has no object" end
		return fp, "free-cities player " .. tostring(id);
	elseif iface == "IGameFalloutManager" then
		local mgr = nil;
		pcall(function() mgr = Game.GetFalloutManager() end);
		if mgr == nil then return nil, "Game.GetFalloutManager() nil" end
		return mgr, "Game.GetFalloutManager()";
	elseif iface == "IMapAreaPortal" then
		local ok, mr = pcall(function() return MapRoutes end);
		if not ok or mr == nil then return nil, "no MapRoutes global" end
		local count = 0;
		pcall(function() count = mr.GetPortalCount() end);
		if (count or 0) == 0 then return nil, "portal count is 0" end
		local portal = nil;
		for i = 0, (count or 0) - 1 do
			pcall(function() portal = mr.GetIndexedPortal(i) end);
			if portal ~= nil then return portal, "MapRoutes portal #" .. tostring(i) .. " of " .. tostring(count) end
		end
		return nil, tostring(count) .. " portals counted but all read nil";
	elseif iface == "IMapTerritory" then
		local ok, t = pcall(function() return Territories end);
		if not ok or t == nil then return nil, "no Territories global" end
		local terr = nil;
		pcall(function()
			local cx, cy = capital:GetX(), capital:GetY();
			terr = t.GetTerritoryAt(cx, cy);
		end);
		if terr == nil then return nil, "no territory at capital plot" end
		return terr, "territory at capital";
	end
	return nil, "unknown iface";
end

local function Run41(owner)
	local p = Players[owner];
	local capital = nil;
	pcall(function() capital = p:GetCities():GetCapitalCity() end);
	local tp, tm, tn = 0, 0, 0;
	for _, e in ipairs(EXPECT41) do
		local inst, note = Reach(e.iface, p, capital);
		if inst == nil then
			tn = tn + #e.methods;
			R41Log(e.iface .. ": NO-INSTANCE (" .. tostring(note) .. ") +" .. tostring(#e.methods) .. " unreached");
		else
			local have = Methods(inst);
			local lp, lm = 0, {};
			for _, m in ipairs(e.methods) do
				if have[m] then lp = lp + 1; else lm[#lm + 1] = m end
			end
			tp, tm = tp + lp, tm + #lm;
			R41Log(e.iface .. ": " .. tostring(lp) .. " present, " .. tostring(#lm) .. " missing via " .. tostring(note)
				.. (#lm > 0 and (" MISSING " .. table.concat(lm, ",")) or ""));
		end
	end
	R41Log(string.format("REACH41 summary: %d present, %d missing, %d no-instance", tp, tm, tn));
end

GameEvents.PlayerTurnStarted.Add(function(owner)
	if R41_DONE then return end
	if owner ~= Game.GetLocalPlayer() then return end
	local okc, cap = pcall(function() return Players[owner]:GetCities():GetCapitalCity() end);
	if not okc or cap == nil then return end   -- wait until the capital exists (districts/territory need it)
	R41_DONE = true;
	local ok, err = pcall(Run41, owner);
	if not ok then R41Log("run failed: " .. tostring(err)) end
end);

-- Manual mid-turn probe for Deal + DealItem (a working deal only exists while
-- the negotiation screen is open, which never coincides with turn start).
-- Called from the Playground "R41: deal check" button; returns a message.
function DevCEPg_R41DealCheck(owner)
	local p = Players[owner];
	local parts = {};
	for _, iface in ipairs({ "IDeal", "IDealItem" }) do
		local spec = nil;
		for _, e in ipairs(EXPECT41) do if e.iface == iface then spec = e break end end
		local inst, note = Reach(iface, p, nil);
		if inst == nil then
			parts[#parts + 1] = iface .. " NO-INSTANCE (" .. tostring(note) .. ")";
		else
			local have = Methods(inst);
			local lp, lm = 0, {};
			for _, m in ipairs(spec.methods) do
				if have[m] then lp = lp + 1 else lm[#lm + 1] = m end
			end
			parts[#parts + 1] = iface .. " " .. tostring(lp) .. " present, " .. tostring(#lm) .. " missing"
				.. (#lm > 0 and (" (" .. table.concat(lm, ",") .. ")") or "");
		end
		R41Log(parts[#parts]);
	end
	return table.concat(parts, " | ");
end

print("[DevCEPg][R41] ready.");
