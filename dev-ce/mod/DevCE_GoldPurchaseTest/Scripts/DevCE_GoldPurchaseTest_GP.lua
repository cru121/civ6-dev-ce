-- Dev CE gold purchase test: gives the human player 1500 gold once (at the start of their first turn),
-- so the gold purchase of Walls / Castle / Spy can be tried without waiting for the treasury.
local PROPERTY :string = "DEVCE_GPTEST_GOLD_GIVEN";

GameEvents.PlayerTurnStarted.Add(function(playerID)
	local pPlayer = Players[playerID];
	if pPlayer == nil or not pPlayer:IsHuman() then return; end
	if pPlayer:GetProperty(PROPERTY) ~= nil then return; end
	pPlayer:SetProperty(PROPERTY, 1);
	pPlayer:GetTreasury():ChangeGoldBalance(1500);
	print("DevCE_GoldPurchaseTest: +1500 gold for player " .. tostring(playerID));
end);
