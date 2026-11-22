-- =====================================================================
--  GWA_Gameplay.lua   (runs in the GAMEPLAY Lua context)
--
--  1. Ownership of archived works.  A great work record only knows its CREATOR (also used by the game for theming), so who an
--     UNSLOTTED work belongs to is stored here, in the Game property GWA_OWNERS: a table with STRING keys (the work's list index as text)
--     and values "owner|eventCode|turn".  (Modders report that numeric table keys can change on reload, hence the strings.)
--     The custom GameCore reports ownership events (a building was removed, a placement failed, a relic was archived); this script drains them
--     with GWArchive.PopEvent() at every turn start and when the UI asks (GWA_DrainCmd), and removes entries for works that are displayed again.
--     Event codes: 1 building removed | 2 placement failed (trade, capture overflow) | 3 relic archived | (4 ghost cleanup, 5 destroyed: owner 63; not yet)
--
--  2. Placing an archived work.  The UI sends an EXECUTE_SCRIPT unit command "GWA_PlaceCmd" (PARAM_X = list index, PARAM_Y = request number); the
--     engine replicates it and raises GameEvents.GWA_PlaceCmd here on every client.  This script checks the owner, calls the native
--     GWArchive.TryPlace and records the answer in player properties so the UI can read it: GWA_CODE, GWA_IDX first, GWA_SEQ last.
--  Codes: 0 placed | 1 no such work | 2 belongs to another player | 3 already displayed
--         4 no city has a free suitable slot | 8 internal failure | 9 native extension / game state missing
-- =====================================================================

print("[GWA][GP] Gameplay script loading...");

local OWNERS_KEY = "GWA_OWNERS";

local function LoadOwners()
	local t = nil;
	pcall(function() t = Game.GetProperty(OWNERS_KEY); end);
	if type(t) ~= "table" then t = {}; end
	return t;
end

-- "owner|code|turn" -> owner, code, turn
local function ParseEntry(e)
	if type(e) ~= "string" then return nil; end
	local o, c, t = string.match(e, "^(%-?%d+)|(%-?%d+)|(%-?%d+)$");
	if o == nil then return nil; end
	return tonumber(o), tonumber(c), tonumber(t);
end

-- The native trade hooks need to know who owns an archived work: mirror the table into the GameCore (it keeps no copy of its own across a reload).
local function SyncOwners(owners)
	if GWArchive == nil or GWArchive.ClearOwners == nil then return; end
	GWArchive.ClearOwners();
	for key, entry in pairs(owners) do
		local idx = tonumber(key);
		local owner = ParseEntry(entry);
		if idx ~= nil and owner ~= nil then GWArchive.SetOwner(idx, owner); end
	end
end

local function DrainEvents(reason)
	if GWArchive == nil or GWArchive.PopEvent == nil then return; end
	local owners = LoadOwners();
	local changed, added, pruned = false, 0, 0;
	for _ = 1, 1000 do
		local work, owner, code, turn = GWArchive.PopEvent();
		if work == nil then break; end
		owners[tostring(work)] = tostring(owner) .. "|" .. tostring(code) .. "|" .. tostring(turn);
		changed = true; added = added + 1;
		print(string.format("[GWA][GP] ownership: work %d -> player %d (event code %d, turn %d)", work, owner, code, turn));
	end
	-- forget works that are on display again
	local drop = {};
	for key, _ in pairs(owners) do
		local idx = tonumber(key);
		if idx == nil or GWArchive.IsHeld(idx) then table.insert(drop, key); end
	end
	for _, key in ipairs(drop) do owners[key] = nil; changed = true; pruned = pruned + 1; end
	if changed then
		Game:SetProperty(OWNERS_KEY, owners);
		print(string.format("[GWA][GP] ownership table updated (%s): +%d, -%d", tostring(reason), added, pruned));
	end
	SyncOwners(owners);
end

-- UI asks for a drain when the Great Works screen opens (so the list is current even if the turn has not changed)
local function OnDrainCmd(eOwner, iUnitID, parameters)
	DrainEvents("UI request");
end
GameEvents.GWA_DrainCmd.Add(OnDrainCmd);

-- Manual cleanup of "ghost" slot entries (slots of buildings a city no longer has; see findings/topics/ghost-great-work-slots.md), asked for from the Great Works screen.
-- Result for the UI: player property GWA_PURGE = "seq|entriesDropped|worksMovedToArchive".
local function OnPurgeCmd(eOwner, iUnitID, parameters)
	local seq;
	pcall(function() seq = parameters and parameters[UnitCommandTypes.PARAM_X]; end);
	local entries, works = 0, 0;
	if GWArchive ~= nil and GWArchive.PurgeGhosts ~= nil then
		local ok, e, w = pcall(GWArchive.PurgeGhosts, eOwner);
		if ok then entries, works = e or 0, w or 0; else print("[GWA][GP] PurgeGhosts threw: " .. tostring(e)); end
	end
	DrainEvents("after ghost cleanup");   -- records the "found in a leftover slot" events right away
	print(string.format("[GWA][GP] ghost cleanup for player %s: %d entr(ies) dropped, %d work(s) moved to the archive", tostring(eOwner), entries, works));
	local player = Players[eOwner];
	if player ~= nil then player:SetProperty("GWA_PURGE", string.format("%d|%d|%d", seq or 0, entries, works)); end
end
GameEvents.GWA_PurgeCmd.Add(OnPurgeCmd);
pcall(function() GameEvents.PlayerTurnStarted.Add(function(pid) DrainEvents("turn start, player " .. tostring(pid)); end); end);

local function OnPlaceCmd(eOwner, iUnitID, parameters)
	local idx, seq;
	pcall(function()
		idx = parameters and parameters[UnitCommandTypes.PARAM_X];
		seq = parameters and parameters[UnitCommandTypes.PARAM_Y];
	end);

	local code = 9;
	if idx ~= nil and GWArchive ~= nil and GWArchive.TryPlace ~= nil then
		DrainEvents("before placing");
		local byProperty = 0;
		local owner, _, _ = ParseEntry(LoadOwners()[tostring(idx)]);
		if owner ~= nil then
			if owner == eOwner then byProperty = 1; else code = 2; end   -- the ownership table names someone else
		end
		if code ~= 2 then
			local ok, result = pcall(GWArchive.TryPlace, eOwner, idx, byProperty);
			if ok and type(result) == "number" then
				code = result;
			else
				code = 8;
				print("[GWA][GP] TryPlace threw or returned a non-number: " .. tostring(result));
			end
		end
	elseif GWArchive == nil then
		print("[GWA][GP] GWArchive is nil: the custom GameCore is not loaded.");
	end

	print(string.format("[GWA][GP] PlaceCmd owner=%s index=%s seq=%s -> code %s",
		tostring(eOwner), tostring(idx), tostring(seq), tostring(code)));

	local player = Players[eOwner];
	if player ~= nil then
		player:SetProperty("GWA_CODE", code);
		player:SetProperty("GWA_IDX", idx or -1);
		player:SetProperty("GWA_SEQ", seq or 0);   -- last: the UI keys on this
	end
end

GameEvents.GWA_PlaceCmd.Add(OnPlaceCmd);

pcall(function() SyncOwners(LoadOwners()); end);   -- after a load: give the native trade hooks the saved table

if GWArchive ~= nil and GWArchive.PopEvent ~= nil then
	print("[GWA][GP] Ready. GWArchive.TryPlace / PopEvent / IsHeld are available.");
elseif GWArchive ~= nil then
	print("[GWA][GP] Ready, but this GameCore has no PopEvent (old build?): ownership events are not collected.");
else
	print("[GWA][GP] Ready, but GWArchive is NOT available (custom GameCore not loaded?).");
end
