-- =====================================================================
--  DevCE_UnitCommandActions_GP.lua   (gameplay Lua context)
--
--  Reads DevCE_UnitCommandActions / DevCE_UnitCommandGrants and hands them to the Dev CE native UnitCommandActions:
--    UnitCommandActions.Define(commandHash, templateIndividualIndex, aiSearchRadius)
--    UnitCommandActions.Grant(commandHash, unitTypeIndex)
--    UnitCommandActions.GrantPromotion(commandHash, promotionIndex)
--    UnitCommandActions.GrantAbility(commandHash, abilityIndex)
--  Runs on every game start and load (the game forgets the commands with the old game).
-- =====================================================================

local LOG = "[DevCEUnitCommandActions] ";

local function PushActions()
	if UnitCommandActions == nil or UnitCommandActions.Define == nil then
		print(LOG .. "Dev CE with UnitCommandActions is not loaded: the commands do nothing");
		return;
	end

	local defined = {};
	local nDefined, nGrants, nSkipped = 0, 0, 0;
	for row in GameInfo.DevCE_UnitCommandActions() do
		local command = GameInfo.UnitCommands[row.CommandType];
		local individual = GameInfo.GreatPersonIndividuals[row.GreatPersonIndividualType];
		if command == nil or individual == nil then
			print(LOG .. "skipped " .. tostring(row.CommandType) .. ": unknown command or individual " .. tostring(row.GreatPersonIndividualType));
		else
			local radius = tonumber(row.AISearchRadius) or 3;
			local ok, err = pcall(UnitCommandActions.Define, command.Hash, individual.Index, radius);
			if ok then
				defined[row.CommandType] = command.Hash;
				nDefined = nDefined + 1;
			else
				print(LOG .. "Define failed for " .. row.CommandType .. ": " .. tostring(err));
			end
		end
	end

	for row in GameInfo.DevCE_UnitCommandGrants() do
		local hash = defined[row.CommandType];
		if hash == nil then
			nSkipped = nSkipped + 1;
		elseif row.UnitType ~= nil then
			local unit = GameInfo.Units[row.UnitType];
			if unit ~= nil then
				UnitCommandActions.Grant(hash, unit.Index);
				nGrants = nGrants + 1;
			else
				print(LOG .. "unknown unit type " .. tostring(row.UnitType));
			end
		elseif row.UnitPromotionType ~= nil then
			local promotion = GameInfo.UnitPromotions[row.UnitPromotionType];
			if promotion ~= nil and UnitCommandActions.GrantPromotion ~= nil then
				UnitCommandActions.GrantPromotion(hash, promotion.Index);   -- only GameInfo indices: the game does not range-check them
				nGrants = nGrants + 1;
			else
				print(LOG .. "unknown promotion " .. tostring(row.UnitPromotionType) .. " (or this Dev CE has no GrantPromotion)");
			end
		elseif row.UnitAbilityType ~= nil then
			local ability = GameInfo.UnitAbilities[row.UnitAbilityType];
			if ability ~= nil and UnitCommandActions.GrantAbility ~= nil then
				UnitCommandActions.GrantAbility(hash, ability.Index);
				nGrants = nGrants + 1;
			else
				print(LOG .. "unknown ability " .. tostring(row.UnitAbilityType) .. " (or this Dev CE has no GrantAbility)");
			end
		else
			nSkipped = nSkipped + 1;   -- a row with no grant column set
		end
	end
	print(LOG .. nDefined .. " commands defined, " .. nGrants .. " grants" .. (nSkipped > 0 and (", " .. nSkipped .. " rows skipped") or ""));
end

PushActions();
