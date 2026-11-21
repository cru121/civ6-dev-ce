-- Dev CE city specialists: applies lock requests sent by the UI (UI/DevCE_CitySpecialists.lua) on the game's thread.
-- The UI sends UI.RequestPlayerOperation(player, PlayerOperations.EXECUTE_SCRIPT, { OnStart = "DevCE_SetSpecialistLock", CityID, SpecialistType, Count });
-- the game calls this handler while it processes the operation.

GameEvents.DevCE_SetSpecialistLock.Add(function(playerID, params)
	if CitySpecialists == nil or not CitySpecialists.IsEnabled() then return; end
	if params == nil or params.CityID == nil or params.SpecialistType == nil or params.Count == nil then return; end
	local city = CityManager.GetCity(playerID, params.CityID);
	if city == nil or city:GetOwner() ~= playerID then return; end
	local count = math.max(0, math.floor(tonumber(params.Count) or 0));
	CitySpecialists.SetLocked(playerID, params.CityID, tostring(params.SpecialistType), count);
end);
