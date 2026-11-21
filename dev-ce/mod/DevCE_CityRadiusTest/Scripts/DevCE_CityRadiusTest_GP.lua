-- Dev CE city radius test: gives the human player 5000 gold once (start of their first turn) to buy tiles with.
local PROPERTY :string = "DEVCE_CRTEST_GOLD_GIVEN";

GameEvents.PlayerTurnStarted.Add(function(playerID)
	local pPlayer = Players[playerID];
	if pPlayer == nil or not pPlayer:IsHuman() then return; end
	if pPlayer:GetProperty(PROPERTY) ~= nil then return; end
	pPlayer:SetProperty(PROPERTY, 1);
	pPlayer:GetTreasury():ChangeGoldBalance(5000);
	print("DevCE_CityRadiusTest: +5000 gold for player " .. tostring(playerID));
end);
