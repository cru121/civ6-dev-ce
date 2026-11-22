-- =====================================================================
--  GWA_Panel.lua   (runs in the UI Lua context)
--
--  An "Archive" button on the Great Works screen. The panel lists great works the local player owns
--  that are displayed in NO slot of anyone's cities. Clicking one asks the game to place it in a city:
--    * success -> the Great Works screen closes and a short message ("toast") says where it went;
--    * failure -> the screen stays open and the panel says why.
--
--  Technique copied from the Great People mod: a new InGame context, detect the screen through the
--  events it raises, ChangeParent our controls into it, and send the action to the gameplay VM with an
--  EXECUTE_SCRIPT unit command (the multiplayer-safe bridge; ExposedMembers is not used).
--  The native GWArchive.TryPlace (custom GameCore) does the authoritative checks and returns a code.
-- =====================================================================

include("InstanceManager");

print("[GWA][UI] Panel script loading...");

local CMD_NAME  = "GWA_PlaceCmd";
local SEQ_PROP  = "GWA_SEQ";
local CODE_PROP = "GWA_CODE";
local MAX_ROWS  = 12;
local MAX_RECORDS = 512;       -- upper bound for the scan of the game-wide list (the bounds-checked owner lookup makes any size safe)
local TOAST_SECONDS = 12;
local RESPONSE_TIMEOUT = 8;

local HOST_PATHS = { "/InGame/GreatWorksOverview" };

local m_RowIM    = InstanceManager:new("GWA_RowInst", "GWA_RowBtn", Controls.GWA_ListStack);
local m_attached = false;
local m_dumped   = false;
local PollPurge;           -- defined below (Trigger uses it)
local m_purge = nil;      -- { pid, seq, checks }: a ghost-slot cleanup request waiting for its answer
local RefreshAfterDrain;           -- defined below (OnPublishComplete uses it)
local m_refreshAfterDrain = false;   -- the Great Works screen asked the gameplay script to update the ownership table: redraw on the next event batch
local m_pending  = nil;        -- { pid, idx, seq, name, waited }
local m_toastLeft = nil;
local m_toastPublishes = nil;   -- fallback timer for the toast: number of game-core event batches left
local TOAST_PUBLISHES = 600;
local Trigger;                  -- defined below; declared here because Refresh() uses it

-- ---------------------------------------------------------------------
--  small helpers
-- ---------------------------------------------------------------------

local function Lookup(key)
	if key == nil then return ""; end
	local ok, text = pcall(Locale.Lookup, key);
	if ok and text ~= nil then return text; end
	return tostring(key);
end

local function AliveIDs()
	local ids = {};
	pcall(function()
		for _, id in ipairs(PlayerManager.GetAliveIDs()) do table.insert(ids, id); end
	end);
	return ids;
end

local function SlotTypeNameFromIndex(slotTypeIndex)
	local name = nil;
	pcall(function()
		for row in GameInfo.GreatWorkSlotTypes() do
			if row.Index == slotTypeIndex then name = row.GreatWorkSlotType; break; end
		end
	end);
	return name;
end

-- Visit every great-work slot of every city of one player.
-- fn(city, cityBldgs, buildingInfo, slotIndex, slotTypeName, greatWorkListIndex)
local function ForEachSlot(pid, fn)
	local player = Players[pid];
	if player == nil then return; end
	pcall(function()
		for _, city in player:GetCities():Members() do
			local bl = city:GetBuildings();
			for bi in GameInfo.Buildings() do
				if bl:HasBuilding(bi.Index) then
					local n = bl:GetNumGreatWorkSlots(bi.Index);
					if n ~= nil and n > 0 then
						for s = 0, n - 1 do
							local gw = bl:GetGreatWorkInSlot(bi.Index, s);
							local slotTypeName = SlotTypeNameFromIndex(bl:GetGreatWorkSlotType(bi.Index, s));
							fn(city, bl, bi, s, slotTypeName, gw);
						end
					end
				end
			end
		end
	end);
end

-- Set of great work list indices displayed in any slot of any living player's cities.
local function CollectSlotted()
	local slotted = {};
	for _, pid in ipairs(AliveIDs()) do
		ForEachSlot(pid, function(city, bl, bi, s, slotTypeName, gw)
			if gw ~= nil and gw >= 0 then slotted[gw] = true; end
		end);
	end
	return slotted;
end

-- Ownership of unslotted works: the gameplay script keeps the Game property GWA_OWNERS (string keys = list index, values "owner|code|turn").
-- A record's own player field is only the CREATOR, so the table wins when it names an owner.
local function LoadOwnersUI()
	local t = nil;
	pcall(function() t = Game.GetProperty("GWA_OWNERS"); end);
	if type(t) ~= "table" then t = {}; end
	return t;
end
local function OwnerFromEntry(e)
	if type(e) ~= "string" then return nil; end
	local o = string.match(e, "^(%-?%d+)|");
	return o ~= nil and tonumber(o) or nil;
end

-- The local player's archived works: they own the record and it is in no slot anywhere.
local function CollectArchived(pid)
	local slotted = CollectSlotted();
	local list = {};
	-- SAFETY (this crashed the game once): Game.GetGreatWorkPlayer is bounds-checked and returns -1 for any index
	-- past the end of the list, but Game.GetGreatWorkDataFromIndex (and the other record readers) read the list
	-- blindly. pcall cannot catch that (it is a native access violation). So only the bounds-checked call is made
	-- for every index; the others are made only for an index that is a real record owned by the local player.
	local owners = LoadOwnersUI();
	for i = 0, MAX_RECORDS - 1 do
		local okOwner, creator = pcall(Game.GetGreatWorkPlayer, i);
		local entryOwner = OwnerFromEntry(owners[tostring(i)]);
		local owner = creator;
		if entryOwner ~= nil then owner = entryOwner; end
		-- a record is read only when the bounds-checked lookup says it exists (creator ~= -1) or the ownership table names it (it only holds real indexes)
		if okOwner and (creator ~= -1 or entryOwner ~= nil) and owner == pid and not slotted[i] then
			local okType, gwType = pcall(Game.GetGreatWorkTypeFromIndex, i);
			local okData, data = pcall(Game.GetGreatWorkDataFromIndex, i);
			if okType and gwType ~= nil and gwType >= 0 and okData and data ~= nil then
				if not m_dumped then
					local keys = "";
					for k, v in pairs(data) do keys = keys .. tostring(k) .. "=" .. tostring(v) .. "  "; end
					print("[GWA][UI] first archived record fields: " .. keys);
					m_dumped = true;
				end
				local info = GameInfo.GreatWorks[gwType];
				if info ~= nil then
					table.insert(list, { index = i, type = gwType, info = info, turn = data.TurnCreated, raw = owners[tostring(i)] });
				end
			end
		end
	end
	return list;
end

-- How/when a work entered the archive, from the ownership property "owner|code|turn" (see GWA_Gameplay.lua for the codes).
local EVENT_TEXT = {
	[1] = "its building was removed",
	[2] = "no room when it was received (trade or capture)",
	[3] = "relic found with no free relic slot",
	[4] = "found in a leftover slot and removed from it",
	[5] = "destroyed",
};
local function DescribeArchiveEvent(raw)
	if type(raw) ~= "string" then return "owner not recorded: shown as its creator's work", nil; end
	local o, c, t = string.match(raw, "^(%-?%d+)|(%-?%d+)|(%-?%d+)$");
	if o == nil then return "owner record unreadable", "GWA_OWNERS entry: " .. raw; end
	local code, turn = tonumber(c), tonumber(t);
	local why = EVENT_TEXT[code] or ("event code " .. c);
	return "archived on turn " .. t .. ": " .. why, "GWA_OWNERS entry: " .. raw .. "  (owner " .. o .. ", event code " .. c .. ", turn " .. t .. ")";
end

local function DescribeWork(entry)
	local name = Lookup(entry.info.Name);
	local objName = "";
	local objRow = GameInfo.GreatWorkObjectTypes[entry.info.GreatWorkObjectType];
	if objRow ~= nil then objName = Lookup(objRow.Name); end
	local year = "";
	if entry.turn ~= nil then
		local ok, y = pcall(Calendar.MakeYearStr, entry.turn);
		if ok and y ~= nil then year = "  (" .. tostring(y) .. ")"; end
	end
	return name, objName, year;
end

-- Where is a work displayed now? Returns cityName, buildingName (or nil).
local function FindHolder(pid, idx)
	local cityName, buildingName = nil, nil;
	ForEachSlot(pid, function(city, bl, bi, s, slotTypeName, gw)
		if gw == idx and cityName == nil then
			cityName = Lookup(city:GetName());
			buildingName = Lookup(bi.Name);
		end
	end);
	return cityName, buildingName;
end

-- ---------------------------------------------------------------------
--  why can't it be placed? (UI-side pre-check; the native call re-checks authoritatively)
--  Returns nil when at least one free slot accepts the work, else a message.
-- ---------------------------------------------------------------------
local function ExplainNoRoom(pid, info)
	local objType = info.GreatWorkObjectType;
	local compatible = {};
	pcall(function()
		for row in GameInfo.GreatWork_ValidSubTypes() do
			if row.GreatWorkObjectType == objType then compatible[row.GreatWorkSlotType] = true; end
		end
	end);

	local total, free, cities = 0, 0, 0;
	local seenCity = {};
	ForEachSlot(pid, function(city, bl, bi, s, slotTypeName, gw)
		local cid = city:GetID();
		if not seenCity[cid] then seenCity[cid] = true; cities = cities + 1; end
		if slotTypeName ~= nil and compatible[slotTypeName] then
			total = total + 1;
			if gw == nil or gw < 0 then free = free + 1; end
		end
	end);

	if free > 0 then return nil; end

	local objRow = GameInfo.GreatWorkObjectTypes[objType];
	local objName = objRow and Lookup(objRow.Name) or tostring(objType);

	if total > 0 then
		return "All " .. tostring(total) .. " display space(s) that accept a " .. objName .. " are full.";
	end

	local names, seenB = {}, {};
	pcall(function()
		for row in GameInfo.Building_GreatWorks() do
			if compatible[row.GreatWorkSlotType] and not seenB[row.BuildingType] then
				seenB[row.BuildingType] = true;
				local b = GameInfo.Buildings[row.BuildingType];
				if b ~= nil and #names < 6 then table.insert(names, Lookup(b.Name)); end
			end
		end
	end);
	local msg = "You have no display space that accepts a " .. objName .. ".";
	if #names > 0 then msg = msg .. " Buildings that have one: " .. table.concat(names, ", ") .. "."; end
	if cities == 0 then msg = "You have no cities to display it in."; end
	return msg;
end

local CODE_MESSAGES = {
	[1] = "The game no longer has a record of this work.",
	[2] = "This work was created for another player.",
	[3] = "The game still holds this work in a slot (possibly a hidden leftover slot). If it is not on display anywhere, press \"Fix hidden slots\" to move it to the archive, then try again.",
	[4] = "No city has a free display space that accepts it.",
	[8] = "The placement call failed inside the game.",
	[9] = "The custom GameCore is not loaded, so the archive cannot place works. Enable this mod's GameCore (and disable other GameCore mods).",
};

-- ---------------------------------------------------------------------
--  toast (shown after the Great Works screen has closed)
-- ---------------------------------------------------------------------
local function ShowToast(text)
	Controls.GWA_ToastLabel:SetText(text .. "  (click to dismiss)");
	Controls.GWA_Toast:SetHide(false);
	m_toastLeft = TOAST_SECONDS;
	m_toastPublishes = TOAST_PUBLISHES;
end

-- ---------------------------------------------------------------------
--  panel content
-- ---------------------------------------------------------------------
local function ShowMessage(text)
	Controls.GWA_Message:SetText(text or "");
	Controls.GWA_OuterStack:CalculateSize();
end

local function FindCarrierUnit(pid)
	local player = Players[pid];
	if player == nil then return nil; end
	local unit = nil;
	pcall(function()
		for _, u in player:GetUnits():Members() do unit = u; break; end
	end);
	return unit;
end

local function OnRowClicked(entry)
	if m_pending ~= nil then return; end   -- one request at a time
	ShowMessage("");
	local pid = Game.GetLocalPlayer();
	local name = DescribeWork(entry);

	local reason = ExplainNoRoom(pid, entry.info);
	if reason ~= nil then
		ShowMessage("Cannot place " .. name .. ": " .. reason);
		print("[GWA][UI] pre-check refused " .. name .. ": " .. reason);
		return;
	end

	local unit = FindCarrierUnit(pid);
	if unit == nil then
		ShowMessage("Cannot send the request: you need at least one unit to carry it.");
		return;
	end

	local prev = 0;
	pcall(function() prev = tonumber(Players[pid]:GetProperty(SEQ_PROP)) or 0; end);
	m_pending = { pid = pid, idx = entry.index, seq = prev + 1, name = name, waited = 0 };

	local params = {};
	params[UnitCommandTypes.PARAM_NAME] = CMD_NAME;
	params[UnitCommandTypes.PARAM_X]    = entry.index;
	params[UnitCommandTypes.PARAM_Y]    = m_pending.seq;
	local ok = pcall(function() UnitManager.RequestCommand(unit, UnitCommandTypes.EXECUTE_SCRIPT, params); end);
	print("[GWA][UI] sent " .. CMD_NAME .. " index=" .. tostring(entry.index) .. " seq=" .. tostring(m_pending.seq) .. " ok=" .. tostring(ok));
	Controls.GWA_Status:SetText("Placing " .. name .. " ...");
end

local function Refresh()
	m_RowIM:ResetInstances();
	ShowMessage("");
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then
		Controls.GWA_Status:SetText("No local player.");
		return 0;
	end

	local archived = CollectArchived(pid);
	local shown = 0;
	for _, entry in ipairs(archived) do
		if shown >= MAX_ROWS then break; end
		local name, objName, year = DescribeWork(entry);
		local inst = m_RowIM:GetInstance();
		local why, tip = DescribeArchiveEvent(entry.raw);
		inst.GWA_RowLabel:SetText(name .. "  -  " .. objName .. year .. "[NEWLINE]" .. why);
		inst.GWA_RowBtn:SetToolTipString(tip or "No ownership entry: this work is listed because it was created for you and is in no slot.");
		inst.GWA_RowBtn:RegisterCallback(Mouse.eLClick, function() OnRowClicked(entry); end);
		inst.GWA_RowBtn:RegisterCallback(Mouse.eMouseEnter, function() Trigger("mouse", 0); end);
		shown = shown + 1;
	end

	Controls.GWA_ListStack:CalculateSize();
	if #archived == 0 then
		Controls.GWA_Status:SetText("No archived great works. Works you own that are in no slot appear here.");
	elseif #archived > MAX_ROWS then
		Controls.GWA_Status:SetText(tostring(#archived) .. " archived works (showing the first " .. tostring(MAX_ROWS) .. "). Click one to place it.");
	else
		Controls.GWA_Status:SetText(tostring(#archived) .. " archived work(s). Click one to place it in a city.");
	end
	Controls.GWA_OuterStack:CalculateSize();
	print("[GWA][UI] refresh: " .. tostring(#archived) .. " archived work(s) for player " .. tostring(pid));
	return #archived;
end

local function HideFrame() Controls.GWA_Frame:SetHide(true); end
local function ToggleFrame()
	if Controls.GWA_Frame:IsHidden() then
		Controls.GWA_Frame:SetHide(false);
		Refresh();
	else
		HideFrame();
	end
end

-- ---------------------------------------------------------------------
--  answer from the gameplay VM (polled every frame while a request is pending)
-- ---------------------------------------------------------------------
local function Resolve(code, foundCity, foundBldg)
	local p = m_pending;
	m_pending = nil;
	print("[GWA][UI] result for index " .. tostring(p.idx) .. ": code " .. tostring(code) .. " (after " .. string.format("%.1f", p.waited) .. "s)");

	if code == 0 then
		local city, bldg = foundCity, foundBldg;
		if city == nil then city, bldg = FindHolder(p.pid, p.idx); end
		local where = (city ~= nil) and (" in " .. city .. (bldg and (" (" .. bldg .. ")") or "")) or "";
		ShowToast("Great work placed: " .. p.name .. " is now on display" .. where .. ".");
		HideFrame();
		pcall(function() LuaEvents.LaunchBar_CloseGreatWorksOverview(); end);
	else
		ShowMessage("Could not place " .. p.name .. ": " .. (CODE_MESSAGES[code] or ("unknown result code " .. tostring(code))));
		Controls.GWA_Status:SetText("Nothing was changed.");
	end
end

local m_seenTrigger = {};
local MAX_CHECKS = 3000;       -- give-up bound when there is no usable clock (counted per poll)

-- Poll the pending request. It is called from several independent triggers (see Trigger below), because the frame update
-- alone was observed NOT to fire in this context (no "update loop" line in Lua.log during the second live test).
local function PollPending(dt)
	if m_pending == nil then return; end
	local p = m_pending;
	p.checks = (p.checks or 0) + 1;
	p.waited = p.waited + (dt or 0);

	-- 1. What the player can actually see: is the work now on display in one of our cities?
	--    (The live test showed the gameplay side placed the work, but the UI never saw the answer written to a player
	--    property, so success is judged from the visible state, not from the property.)
	local city, bldg = FindHolder(p.pid, p.idx);
	if city ~= nil then
		Resolve(0, city, bldg);
		return;
	end

	-- 2. The gameplay script's reason code (player property), if the UI can see it yet.
	local seq = nil;
	pcall(function() seq = tonumber(Players[p.pid]:GetProperty(SEQ_PROP)); end);
	if seq == p.seq then
		local code = 9;
		pcall(function() code = tonumber(Players[p.pid]:GetProperty(CODE_PROP)) or 9; end);
		Resolve(code);
		return;
	end

	-- Diagnostics: the first few checks, then every 50th.
	if p.checks <= 5 or p.checks % 50 == 0 then
		print("[GWA][UI] waiting (check " .. tostring(p.checks) .. ", " .. string.format("%.1f", p.waited) .. "s) for index " .. tostring(p.idx)
			.. ": seq property=" .. tostring(seq) .. " (want " .. tostring(p.seq) .. "), on display=no");
	end

	if p.waited > RESPONSE_TIMEOUT or p.checks > MAX_CHECKS then
		m_pending = nil;
		Refresh();   -- redraw the list from the current state (this clears the message line, so the message is set after)
		ShowMessage("Could not confirm whether " .. p.name .. " was placed. Please close and reopen the Great Works screen to see the current state.");
		Controls.GWA_Status:SetText("Not confirmed: close and reopen this screen.");
		print("[GWA][UI] gave up waiting for index " .. tostring(p.idx));
	end
end

-- ---------------------------------------------------------------------
--  relic popup fix: the game's "great work produced" popup for a relic always says "Displayed in <building>, <city>", taken from the
--  event parameters, even for a relic the archive hook created without a slot. When the relic is held by no slot, replace that line.
--  The popup lives in another context: reach its label with ContextPtr:LookUpControl (path from InGame.xml).
-- ---------------------------------------------------------------------
local SHOWCASE_PATH = "/InGame/GreatWorkShowcase";
local ARCHIVED_PLACE_TEXT = "Added to your Great Works archive (not on display yet)";
local RELIC_FIX_MAX_TRIGGERS = 900;
local m_relicFix = nil;   -- { seenShown = bool, tries = n }

local function TryFixRelicPopup()
	if m_relicFix == nil then return; end
	m_relicFix.tries = m_relicFix.tries + 1;
	local root = ContextPtr:LookUpControl(SHOWCASE_PATH);
	local label = ContextPtr:LookUpControl(SHOWCASE_PATH .. "/CreatedPlace");
	if root == nil or label == nil then
		if m_relicFix.tries == 1 then print("[GWA][UI] relic popup fix: showcase controls not found (root=" .. tostring(root) .. " label=" .. tostring(label) .. ")"); end
	elseif not root:IsHidden() then
		label:SetText(ARCHIVED_PLACE_TEXT);
		if not m_relicFix.seenShown then print("[GWA][UI] relic popup fix: replaced the location line"); end
		m_relicFix.seenShown = true;
	elseif m_relicFix.seenShown then
		m_relicFix = nil;   -- the popup was shown and has been closed: done (a later normal popup must not be touched)
		return;
	end
	if m_relicFix ~= nil and m_relicFix.tries > RELIC_FIX_MAX_TRIGGERS then
		print("[GWA][UI] relic popup fix: gave up (popup never seen)");
		m_relicFix = nil;
	end
end

local function OnShowRelicCreated(playerID, creatorID, cityX, cityY, buildingID, greatWorkIndex)
	print("[GWA][UI] relic popup event: player=" .. tostring(playerID) .. " greatWorkIndex=" .. tostring(greatWorkIndex) .. " building=" .. tostring(buildingID));
	if playerID ~= Game.GetLocalPlayer() then return; end
	local city, bldg = FindHolder(playerID, greatWorkIndex);
	if city ~= nil then return; end   -- really displayed somewhere: the popup is right
	print("[GWA][UI] relic " .. tostring(greatWorkIndex) .. " is held by no slot: will fix the popup's location line");
	m_relicFix = { seenShown = false, tries = 0 };
	TryFixRelicPopup();   -- in case the showcase handler already ran; otherwise the polls below catch it
end

-- Diagnostic: does the game create (and auto-activate) a relic notification for every relic, including the archived ones?
local function OnNotificationAddedDiag(playerID, notificationID)
	if playerID ~= Game.GetLocalPlayer() then return; end
	local ok, err = pcall(function()
		local n = NotificationManager.Find(playerID, notificationID);
		if n == nil then return; end
		if n:GetType() ~= NotificationTypes.RELIC_CREATED then return; end
		print("[GWA][UI] notification added: RELIC_CREATED id=" .. tostring(notificationID) .. " greatWorkIndex=" .. tostring(n:GetValue("GreatWorkIndex"))
			.. " building=" .. tostring(n:GetValue("BuildingID")) .. " visibleInUI=" .. tostring(n:IsVisibleInUI()) .. " dismissed=" .. tostring(n:IsDismissed()));
	end);
	if not ok then print("[GWA][UI] notification diag failed: " .. tostring(err)); end
end

-- ---------------------------------------------------------------------
--  manual cleanup of leftover slot entries ("Fix hidden slots" button on the Great Works screen)
-- ---------------------------------------------------------------------
local PURGE_CMD, PURGE_PROP = "GWA_PurgeCmd", "GWA_PURGE";
local PURGE_MAX_CHECKS = 3000;

local function ParsePurge(raw)
	if type(raw) ~= "string" then return nil; end
	local s, e, w = string.match(raw, "^(%-?%d+)|(%-?%d+)|(%-?%d+)$");
	if s == nil then return nil; end
	return tonumber(s), tonumber(e), tonumber(w);
end

local function OnFixClicked()
	if m_purge ~= nil or m_pending ~= nil then return; end
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then return; end
	local unit = FindCarrierUnit(pid);
	if unit == nil then ShowMessage("Cannot send the request: you need at least one unit to carry it."); return; end
	local prev = 0;
	pcall(function() prev = ParsePurge(Players[pid]:GetProperty(PURGE_PROP)) or 0; end);
	m_purge = { pid = pid, seq = prev + 1, checks = 0 };
	local params = {};
	params[UnitCommandTypes.PARAM_NAME] = PURGE_CMD;
	params[UnitCommandTypes.PARAM_X] = m_purge.seq;
	local ok = pcall(function() UnitManager.RequestCommand(unit, UnitCommandTypes.EXECUTE_SCRIPT, params); end);
	print("[GWA][UI] sent " .. PURGE_CMD .. " seq=" .. tostring(m_purge.seq) .. " ok=" .. tostring(ok));
	if not ok then m_purge = nil; end
end

PollPurge = function()
	if m_purge == nil then return; end
	m_purge.checks = m_purge.checks + 1;
	local seq, entries, works = nil, nil, nil;
	pcall(function() seq, entries, works = ParsePurge(Players[m_purge.pid]:GetProperty(PURGE_PROP)); end);
	if seq == m_purge.seq then
		m_purge = nil;
		local text;
		if entries == 0 then text = "No leftover slots found.";
		else text = "Removed " .. tostring(entries) .. " leftover slot(s); " .. tostring(works) .. " great work(s) moved to your archive."; end
		print("[GWA][UI] ghost cleanup answer: " .. text);
		ShowToast(text);
		m_refreshAfterDrain = true;   -- redraw the archive list and the count on the next event batch
	elseif m_purge.checks > PURGE_MAX_CHECKS then
		m_purge = nil;
		ShowToast("The cleanup request was not confirmed. Close and reopen the Great Works screen to see the current state.");
	end
end

Trigger = function(name, dt)
	if not m_seenTrigger[name] then
		m_seenTrigger[name] = true;
		print("[GWA][UI] trigger '" .. name .. "' is firing");
	end
	PollPending(dt);
	PollPurge();
	TryFixRelicPopup();
end

local function HideToast(reason)
	print("[GWA][UI] toast hidden (" .. tostring(reason or "unknown") .. "), seconds left=" .. tostring(m_toastLeft) .. ", event batches left=" .. tostring(m_toastPublishes));
	m_toastLeft = nil;
	m_toastPublishes = nil;
	Controls.GWA_Toast:SetHide(true);
end

-- Frame update: polls, and times the toast.
local function OnUpdate(dt)
	Trigger("update", dt);
	if m_toastLeft ~= nil then
		m_toastLeft = m_toastLeft - dt;
		if m_toastLeft <= 0 then HideToast("frame timer"); end
	end
end

-- "The game core has just published a batch of events" (the base game's own always-on panels use this): also polls, and
-- is the toast's fallback timer if the frame update never fires.
-- ---------------------------------------------------------------------
--  "archived" toast: the game sends at most one relic notification at a time (a second relic in the same turn gets none), so the
--  panel watches the archive itself and announces every work that newly appears in it. The first check only records the
--  current contents (nothing is announced for works that were already archived, e.g. after loading a save).
-- ---------------------------------------------------------------------
local ARCHIVE_CHECK_EVERY = 15;          -- event batches between checks (the scan walks every city and record)
local m_knownArchived = nil;             -- set of list indexes seen in the archive at the last check
local m_archiveCheckCount = 0;

local function CheckNewArchived()
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then return; end
	local ok, list = pcall(CollectArchived, pid);
	if not ok or list == nil then return; end
	local nowSet, fresh = {}, {};
	for _, entry in ipairs(list) do
		nowSet[entry.index] = true;
		if m_knownArchived ~= nil and not m_knownArchived[entry.index] then table.insert(fresh, entry); end
	end
	m_knownArchived = nowSet;
	if #fresh == 0 then return; end
	if m_pending ~= nil then return; end   -- a placement is in progress: its own message wins
	local names = {};
	for i, entry in ipairs(fresh) do
		if i <= 3 then table.insert(names, (DescribeWork(entry))); end
	end
	local text;
	if #fresh == 1 then
		text = "Added to your Great Works archive: " .. names[1] .. ".";
	else
		text = tostring(#fresh) .. " great works added to your archive: " .. table.concat(names, ", ") .. (#fresh > 3 and ", ..." or "") .. ".";
	end
	print("[GWA][UI] archive toast: " .. text);
	ShowToast(text);
end

local function OnPublishComplete()
	Trigger("publish", 0);
	if m_refreshAfterDrain then RefreshAfterDrain(); end
	m_archiveCheckCount = m_archiveCheckCount + 1;
	if m_archiveCheckCount % ARCHIVE_CHECK_EVERY == 1 then CheckNewArchived(); end
	if m_toastPublishes ~= nil then
		m_toastPublishes = m_toastPublishes - 1;
		if m_toastPublishes <= 0 then HideToast("event-batch fallback timer"); end
	end
end

-- ---------------------------------------------------------------------
--  attach into the Great Works screen, driven by its own events
-- ---------------------------------------------------------------------
local function TryAttach()
	if m_attached then return true; end
	for _, path in ipairs(HOST_PATHS) do
		local host = nil;
		pcall(function() host = ContextPtr:LookUpControl(path); end);
		if host ~= nil then
			local ok = pcall(function() Controls.GWA_Root:ChangeParent(host); end);
			if ok then
				m_attached = true;
				print("[GWA][UI] attached to " .. path);
				return true;
			end
		end
	end
	print("[GWA][UI] ATTACH FAILED: Great Works screen host not found.");
	return false;
end

local DRAIN_CMD = "GWA_DrainCmd";
local function RequestDrain()
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then return; end
	local unit = FindCarrierUnit(pid);
	if unit == nil then return; end
	local params = {};
	params[UnitCommandTypes.PARAM_NAME] = DRAIN_CMD;
	local ok = pcall(function() UnitManager.RequestCommand(unit, UnitCommandTypes.EXECUTE_SCRIPT, params); end);
	m_refreshAfterDrain = ok;
	print("[GWA][UI] asked the gameplay script to update the ownership table, ok=" .. tostring(ok));
end

RefreshAfterDrain = function()
	m_refreshAfterDrain = false;
	local pid = Game.GetLocalPlayer();
	if pid == nil or pid < 0 then return; end
	if not Controls.GWA_Frame:IsHidden() and m_pending == nil then
		Refresh();
	end
	Controls.GWA_TabButton:SetText("Archive (" .. tostring(#CollectArchived(pid)) .. ")");
end

local function OnGreatWorksOpened()
	HideToast("screen opened");
	TryAttach();
	RequestDrain();
	local pid = Game.GetLocalPlayer();
	local n = 0;
	if pid ~= nil and pid >= 0 then n = #CollectArchived(pid); end
	Controls.GWA_TabButton:SetText("Archive (" .. tostring(n) .. ")");
	Controls.GWA_TabButton:SetHide(false);
	HideFrame();
	ShowMessage("");
	print("[GWA][UI] Great Works screen opened; " .. tostring(n) .. " archived work(s).");
end

local function OnGreatWorksClosed()
	Controls.GWA_TabButton:SetHide(true);
	HideFrame();
	m_pending = nil;
end

local function Initialize()
	Controls.GWA_TabButton:SetText("Archive");
	Controls.GWA_TabButton:SetToolTipString("Great works you own that are displayed in no slot. Click one to place it in a city.");
	Controls.GWA_TabButton:RegisterCallback(Mouse.eLClick, ToggleFrame);
	Controls.GWA_FixButton:SetText("Fix hidden slots");
	Controls.GWA_FixButton:SetToolTipString("Looks for leftover display slots of buildings a city no longer has (a work placed there is invisible on this screen) and moves any works in them to the Archive.");
	Controls.GWA_FixButton:RegisterCallback(Mouse.eLClick, OnFixClicked);
	Controls.GWA_FixButton:RegisterCallback(Mouse.eMouseEnter, function() Trigger("mouse", 0); end);
	Controls.GWA_ToastClick:RegisterCallback(Mouse.eLClick, function() HideToast("clicked"); end);

	-- Mouse-over on the tab button is a third, manual trigger for the poll.
	Controls.GWA_TabButton:RegisterCallback(Mouse.eMouseEnter, function() Trigger("mouse", 0); end);

	LuaEvents.GreatWorks_OpenGreatWorks.Add(OnGreatWorksOpened);
	LuaEvents.GreatWorks_CloseGreatWorks.Add(OnGreatWorksClosed);
	LuaEvents.NotificationPanel_ShowRelicCreated.Add(OnShowRelicCreated);
	Events.NotificationAdded.Add(OnNotificationAddedDiag);
	Events.GameCoreEventPublishComplete.Add(OnPublishComplete);

	-- Make sure this context itself is shown (a hidden context gets no frame updates), and say what state it is in.
	pcall(function() ContextPtr:SetHide(false); end);
	local hidden = "?";
	pcall(function() hidden = tostring(ContextPtr:IsHidden()); end);
	ContextPtr:SetUpdate(OnUpdate);

	print("[GWA][UI] Initialized (context hidden=" .. hidden .. "; waiting for the Great Works screen to open).");
end

Initialize();
