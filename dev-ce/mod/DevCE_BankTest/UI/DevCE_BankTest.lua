-- Dev CE soundboard (UI context). Buttons call UI.PlaySound(<Wwise event name>); every call is also printed to Lua.log.
-- BEEP / CONTROL: events of the hand-built bank (Platforms/Windows/Audio/DevCE_Test_Bank.bnk).
-- Leader theme buttons: do what the game's diplomacy screen does for a leader theme: set the switches LEADER_SCREEN_CIVILIZATION and Game_Location,
-- then post Play_Leader_Music (Stop_Leader_Music to end it). The new-civ button uses a civilization name that only a mod bank can add.
print("DevCE_BankTest: loading soundboard...")

local function Play(evt)
	if evt == nil or evt == "" then Controls.Sb_Status:SetText("Type an event name first."); return end
	print("DevCE_BankTest: UI.PlaySound(" .. evt .. ")")
	local ok, err = pcall(function() UI.PlaySound(evt) end)
	Controls.Sb_Status:SetText((ok and "Played: " or "ERROR: ") .. evt .. (ok and "" or (" " .. tostring(err))))
end

-- Our banks are listed in the [FMV] section of DevCETestBanks.ini: the game loads that group only on request (group 5 = BANKS_FMV, as the game's own scripts use it).
-- Load / Unload re-reads the bank files from disk, so a rebuilt bank can be tried without restarting the game.
local BANKS_FMV = 5
local function BankGroup(load)
	local ok, err = pcall(function()
		if load then UI.LoadSoundBankGroup(BANKS_FMV) else UI.UnloadSoundBankGroup(BANKS_FMV) end
	end)
	print("DevCE_BankTest: " .. (load and "LoadSoundBankGroup" or "UnloadSoundBankGroup") .. "(5) ok=" .. tostring(ok) .. (ok and "" or (" " .. tostring(err))))
	Controls.Sb_Status:SetText((load and "Load" or "Unload") .. " banks: " .. (ok and "called" or ("ERROR " .. tostring(err))))
end

-- Era test: our own switch container (Play_DevCE_EraTest) picks a sound by the game's global Game_Location switch (1/2/3/4 beeps = Ancient/Medieval/Industrial/Modern).
local function Era(value)
	local ok, err = pcall(function()
		if value then UI.SetSoundSwitchValue("Game_Location", value) end
		UI.PlaySound("Play_DevCE_EraTest")
	end)
	print("DevCE_BankTest: era test value=" .. tostring(value) .. " ok=" .. tostring(ok) .. (ok and "" or (" " .. tostring(err))))
	Controls.Sb_Status:SetText("Era test " .. (value or "(game's own era)") .. ": " .. (ok and "played" or ("ERROR " .. tostring(err))))
end

local function OnUpdate()
	-- the game creates additional contexts hidden; keep this one shown
	if ContextPtr:IsHidden() then ContextPtr:SetHide(false) end
end

local function Initialize()
	local presets = {
		{ "Sb_Beep",    "Play_DevCE_BankTest_Beep" },
		{ "Sb_Control", "Play_DevCE_BankTest_Control" },
	}
	for _, p in ipairs(presets) do
		local id, evt = p[1], p[2]
		Controls[id]:RegisterCallback(Mouse.eLClick, function() Play(evt) end)
	end
	Controls.Sb_EraAnc:RegisterCallback(Mouse.eLClick, function() Era("Ancient_Era") end)
	Controls.Sb_EraMed:RegisterCallback(Mouse.eLClick, function() Era("Medieval_Era") end)
	Controls.Sb_EraInd:RegisterCallback(Mouse.eLClick, function() Era("Industrial_Era") end)
	Controls.Sb_EraMod:RegisterCallback(Mouse.eLClick, function() Era("Modern_Era") end)
	Controls.Sb_EraNow:RegisterCallback(Mouse.eLClick, function() Era(nil) end)
	Controls.Sb_Load:RegisterCallback(Mouse.eLClick, function() BankGroup(true) end)
	Controls.Sb_Unload:RegisterCallback(Mouse.eLClick, function() BankGroup(false) end)
	Controls.Sb_PlayTyped:RegisterCallback(Mouse.eLClick, function() Play(Controls.Sb_Edit:GetText()) end)
	Controls.Sb_Edit:RegisterCommitCallback(function() Play(Controls.Sb_Edit:GetText()) end)
	ContextPtr:SetHide(false)
	ContextPtr:SetUpdate(OnUpdate)
	if Events.LoadScreenClose ~= nil then Events.LoadScreenClose.Add(function() ContextPtr:SetHide(false) end) end
	print("DevCE_BankTest: soundboard ready")
end

Initialize()
