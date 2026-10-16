-- Dev CE soundboard (UI context). Buttons call UI.PlaySound(<Wwise event name>); every call is also printed to Lua.log.
-- BEEP / CONTROL: events of the hand-built bank (Platforms/Windows/Audio/DevCE_Test_Bank.bnk).
-- CLONE / EXTRACT: events of two banks derived from an official bank (known to work); UI_Screen_Open is a game event for comparison.
print("DevCE_BankTest: loading soundboard...")

local function Play(evt)
	if evt == nil or evt == "" then Controls.Sb_Status:SetText("Type an event name first."); return end
	print("DevCE_BankTest: UI.PlaySound(" .. evt .. ")")
	local ok, err = pcall(function() UI.PlaySound(evt) end)
	Controls.Sb_Status:SetText((ok and "Played: " or "ERROR: ") .. evt .. (ok and "" or (" " .. tostring(err))))
end

local function OnUpdate()
	-- the game creates additional contexts hidden; keep this one shown
	if ContextPtr:IsHidden() then ContextPtr:SetHide(false) end
end

local function Initialize()
	local presets = {
		{ "Sb_Beep",    "Play_DevCE_BankTest_Beep" },
		{ "Sb_Control", "Play_DevCE_BankTest_Control" },
		{ "Sb_Clone",   "Play_DevCE_BankTest_Clone" },
		{ "Sb_Extract", "Play_DevCE_BankTest_Extract" },
		{ "Sb_Open",    "UI_Screen_Open" },
	}
	for _, p in ipairs(presets) do
		local id, evt = p[1], p[2]
		Controls[id]:RegisterCallback(Mouse.eLClick, function() Play(evt) end)
	end
	Controls.Sb_PlayTyped:RegisterCallback(Mouse.eLClick, function() Play(Controls.Sb_Edit:GetText()) end)
	Controls.Sb_Edit:RegisterCommitCallback(function() Play(Controls.Sb_Edit:GetText()) end)
	ContextPtr:SetHide(false)
	ContextPtr:SetUpdate(OnUpdate)
	if Events.LoadScreenClose ~= nil then Events.LoadScreenClose.Add(function() ContextPtr:SetHide(false) end) end
	print("DevCE_BankTest: soundboard ready")
end

Initialize()
