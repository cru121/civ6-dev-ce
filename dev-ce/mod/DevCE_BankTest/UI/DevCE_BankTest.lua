-- When the game view finishes loading: plays a known game sound (UI_Multi_Turn_Movement_Alert), so you know sound works.
-- At the start of every turn after that: plays the BEEP from our hand-built bank (Play_DevCE_BankTest_Beep).
-- (A per-frame timer did not run in this context, so there is no sequencing.) Every call is printed to Lua.log.
local function Play(evt, what)
	print("DevCE_BankTest: " .. what .. " -> UI.PlaySound(" .. evt .. ")")
	UI.PlaySound(evt)
end
Events.LoadGameViewStateDone.Add(function() Play("UI_Multi_Turn_Movement_Alert", "game alert") end)
Events.LocalPlayerTurnBegin.Add(function() Play("Play_DevCE_BankTest_Beep", "BEEP (our bank)") end)
print("DevCE_BankTest: registered")
