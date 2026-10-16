-- Plays the event from the hand-built bank (Platforms/Windows/Audio/DevCE_Test_Bank.bnk) every turn start. UI context only.
local function OnTurnBegin()
	print("DevCE_BankTest: UI.PlaySound(Play_DevCE_BankTest_Beep)")
	UI.PlaySound("Play_DevCE_BankTest_Beep")
end
Events.LocalPlayerTurnBegin.Add(OnTurnBegin)
print("DevCE_BankTest: registered")
