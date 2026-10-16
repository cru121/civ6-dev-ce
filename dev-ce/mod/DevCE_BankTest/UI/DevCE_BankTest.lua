-- Turn start: plays a known game sound and our bank's beep alternately, 3 s apart, so you can tell them apart:
--   t=0 game alert, t=3 BEEP, t=6 BEEP, t=9 game alert, t=12 BEEP. Each step is also printed to Lua.log.
local steps = {
	{ 0,  "UI_Multi_Turn_Movement_Alert", "game alert" },
	{ 3,  "Play_DevCE_BankTest_Beep",     "BEEP (our bank)" },
	{ 6,  "Play_DevCE_BankTest_Beep",     "BEEP (our bank)" },
	{ 9,  "UI_Multi_Turn_Movement_Alert", "game alert" },
	{ 12, "Play_DevCE_BankTest_Beep",     "BEEP (our bank)" },
}
local clock, nextStep = nil, 1
local function OnUpdate(dt)
	if clock == nil then return end
	clock = clock + dt
	while nextStep <= #steps and clock >= steps[nextStep][1] do
		local s = steps[nextStep]
		print("DevCE_BankTest: t=" .. s[1] .. " " .. s[3] .. " -> UI.PlaySound(" .. s[2] .. ")")
		UI.PlaySound(s[2])
		nextStep = nextStep + 1
	end
	if nextStep > #steps then clock = nil end
end
local function OnTurnBegin()
	clock, nextStep = 0, 1
end
ContextPtr:SetUpdate(OnUpdate)
Events.LocalPlayerTurnBegin.Add(OnTurnBegin)
print("DevCE_BankTest: registered")
