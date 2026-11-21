-- ===========================================================================
--	Dev CE GoldPurchase: the GetData wrapper shared by the ProductionPanel replacements of this mod.
--	Include it AFTER the panel it should extend (base ProductionPanel, or another mod's replacement such as productionpanel_CQUI):
--		include("ProductionPanel");  include("DevCE_GoldPurchase_Wrap");
--	It wraps the GetData that is defined at that point and adds the unlocked build-only items to the gold purchase lists.
-- ===========================================================================
local BASE_DEVCE_GetData = GetData;
local PREFIX :string = "DEVCE_GOLD_PURCHASE_";

local function Has(pCity:table, sName)
	if sName == nil or sName == "" then return false; end
	local value = pCity:GetProperty(PREFIX .. sName);
	return type(value) == "number" and value >= 1;
end

-- Same keys as the engine rule (GoldPurchase.cpp); the engine has the last word through CityManager.CanStartCommand.
local function UnitUnlocked(pCity:table, row:table)
	return Has(pCity, "ALL_UNITS") or Has(pCity, row.UnitType) or Has(pCity, row.PromotionClass);
end

local function BuildingUnlocked(pCity:table, row:table)
	return Has(pCity, "ALL_BUILDINGS") or Has(pCity, row.BuildingType) or Has(pCity, row.PrereqDistrict);
end

function GetData()
	local data = BASE_DEVCE_GetData();
	if data == nil then return data; end
	local pCity :table = data.City or UI.GetHeadSelectedCity();
	local pPlayer :table = Players[Game.GetLocalPlayer()];
	if pCity == nil or pPlayer == nil then return data; end
	local pTreasury :table = pPlayer:GetTreasury();

	for row in GameInfo.Buildings() do
		if row.PurchaseYield == nil and not row.IsWonder and BuildingUnlocked(pCity, row) then
			local isAllowed, kBldg = ComposeBldgForPurchase( row, pCity, "YIELD_GOLD", pTreasury, "LOC_BUILDING_INSUFFICIENT_FUNDS" );
			if isAllowed then
				table.insert( data.BuildingPurchases, kBldg );
			end
		end
	end

	for row in GameInfo.Units() do
		if row.PurchaseYield == nil and UnitUnlocked(pCity, row) then
			local isAllowed, kUnit = ComposeUnitForPurchase( row, pCity, "YIELD_GOLD", pTreasury, "LOC_BUILDING_INSUFFICIENT_FUNDS" );
			if isAllowed then
				table.insert( data.UnitPurchases, kUnit );
			end
		end
	end
	return data;
end
