-- =====================================================================
--  DevCE_TerrainObjects_Gameplay.lua   (runs in the GAMEPLAY Lua context)
--
--  Handlers of the "Tile objects" panel of the Dev CE Playground: features, resources and improvements on the tile of the selected unit (else the first unit).
--  Same transport as the other buttons (UI -> EXECUTE_SCRIPT unit command -> GameEvents.<name>(owner, unit id, parameters)); the answer goes back through the
--  player properties DEVCEPG_MSG / DEVCEPG_SEQ that the UI script shows in the status line.
--  With the Dev CE test build (terrain view patch) the 3D map shows these changes at once; without it features, resources and improvements still redraw
--  (they are separate objects), but a terrain change under them only shows after a save + load.
-- =====================================================================

print("[DevCEPg][OBJ] loading...");

local seq = 0;
local function Report(owner, msg)
	seq = seq + 1000 + math.random(1, 999);
	print("[DevCEPg][OBJ] " .. msg);
	pcall(function()
		Players[owner]:SetProperty("DEVCEPG_MSG", msg);
		Players[owner]:SetProperty("DEVCEPG_SEQ", seq .. "o");
	end);
end

local function UnitPlot(owner, unitID)
	local u = nil;
	pcall(function() u = Players[owner]:GetUnits():FindID(unitID); end);
	if u == nil then return nil, "no carrier unit (select a unit first)"; end
	local plot = Map.GetPlot(u:GetX(), u:GetY());
	if plot == nil then return nil, "no plot under the unit"; end
	return plot, nil;
end

local function Strip(prefix, name) return (tostring(name):gsub("^" .. prefix, "")); end

local function FeatureName(plot)
	local f = plot:GetFeatureType();
	if f == nil or f < 0 then return "none"; end
	local row = GameInfo.Features[f];
	return row and Strip("FEATURE_", row.FeatureType) or tostring(f);
end
local function ResourceName(plot)
	local r = plot:GetResourceType();
	if r == nil or r < 0 then return "none"; end
	local row = GameInfo.Resources[r];
	return row and Strip("RESOURCE_", row.ResourceType) or tostring(r);
end
local function ImprovementName(plot)
	local i = plot:GetImprovementType();
	if i == nil or i < 0 then return "none"; end
	local row = GameInfo.Improvements[i];
	return row and Strip("IMPROVEMENT_", row.ImprovementType) or tostring(i);
end
local function TerrainShort(plot)
	local row = GameInfo.Terrains[plot:GetTerrainType()];
	return row and Strip("TERRAIN_", row.TerrainType) or "?";
end

-- ---------------------------------------------------------------- features
local function SetFeature(owner, unitID, typeName)
	local plot, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Feature: " .. why); return; end
	local before = FeatureName(plot);
	local ok, err = pcall(function()
		if typeName == nil then
			TerrainBuilder.SetFeatureType(plot, -1);
		else
			local row = GameInfo.Features[typeName];
			if row == nil then error(typeName .. " not found"); end
			if TerrainBuilder.CanHaveFeature ~= nil and not TerrainBuilder.CanHaveFeature(plot, row.Index) then
				error(Strip("FEATURE_", typeName) .. " cannot grow on " .. TerrainShort(plot));
			end
			TerrainBuilder.SetFeatureType(plot, row.Index);
		end
	end);
	Report(owner, "Feature " .. (typeName and Strip("FEATURE_", typeName) or "OFF") .. " at " .. plot:GetX() .. "," .. plot:GetY() .. ": " .. (ok and "ok" or ("failed: " .. tostring(err)))
		.. "; " .. before .. " -> " .. FeatureName(plot) .. ".");
end
GameEvents.DevCEPg_O_Jungle.Add(function(owner, unitID) SetFeature(owner, unitID, "FEATURE_JUNGLE"); end);
GameEvents.DevCEPg_O_Marsh.Add(function(owner, unitID) SetFeature(owner, unitID, "FEATURE_MARSH"); end);
GameEvents.DevCEPg_O_FeatOff.Add(function(owner, unitID) SetFeature(owner, unitID, nil); end);

-- ---------------------------------------------------------------- resources
local function SetResource(owner, unitID, typeName)
	local plot, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Resource: " .. why); return; end
	local before = ResourceName(plot);
	local ok, err = pcall(function()
		if typeName == nil then
			ResourceBuilder.SetResourceType(plot, -1);
		else
			local row = GameInfo.Resources[typeName];
			if row == nil then error(typeName .. " not found in this game"); end
			if ResourceBuilder.CanHaveResource ~= nil and not ResourceBuilder.CanHaveResource(plot, row.Index) then
				error(Strip("RESOURCE_", typeName) .. " cannot be placed on " .. TerrainShort(plot) .. " (feature " .. FeatureName(plot) .. ")");
			end
			ResourceBuilder.SetResourceType(plot, row.Index, 1);
		end
	end);
	Report(owner, "Resource " .. (typeName and Strip("RESOURCE_", typeName) or "OFF") .. " at " .. plot:GetX() .. "," .. plot:GetY() .. ": " .. (ok and "ok" or ("failed: " .. tostring(err)))
		.. "; " .. before .. " -> " .. ResourceName(plot) .. ".");
end
local RESOURCES = {
	{ "DevCEPg_O_R_Wheat", "RESOURCE_WHEAT" }, { "DevCEPg_O_R_Cattle", "RESOURCE_CATTLE" }, { "DevCEPg_O_R_Horses", "RESOURCE_HORSES" },
	{ "DevCEPg_O_R_Copper", "RESOURCE_COPPER" }, { "DevCEPg_O_R_Iron", "RESOURCE_IRON" }, { "DevCEPg_O_R_Stone", "RESOURCE_STONE" },
};
for _, r in ipairs(RESOURCES) do
	local name = r[2];
	GameEvents[r[1]].Add(function(owner, unitID) SetResource(owner, unitID, name); end);
end
GameEvents.DevCEPg_O_R_Off.Add(function(owner, unitID) SetResource(owner, unitID, nil); end);

-- ---------------------------------------------------------------- improvements
local function SetImprovement(owner, unitID, typeName)
	local plot, why = UnitPlot(owner, unitID);
	if plot == nil then Report(owner, "Improvement: " .. why); return; end
	local before = ImprovementName(plot);
	local who = plot:GetOwner();
	if who == nil or who < 0 then who = owner; end
	local ok, err = pcall(function()
		if typeName == nil then
			ImprovementBuilder.SetImprovementType(plot, -1, -1);
		else
			local row = GameInfo.Improvements[typeName];
			if row == nil then error(typeName .. " not found in this game"); end
			if ImprovementBuilder.CanHaveImprovement ~= nil and not ImprovementBuilder.CanHaveImprovement(plot, row.Index, who) then
				error(Strip("IMPROVEMENT_", typeName) .. " is not allowed on " .. TerrainShort(plot) .. " (feature " .. FeatureName(plot) .. ", resource " .. ResourceName(plot) .. ")");
			end
			ImprovementBuilder.SetImprovementType(plot, row.Index, who);
		end
	end);
	Report(owner, "Improvement " .. (typeName and Strip("IMPROVEMENT_", typeName) or "OFF") .. " at " .. plot:GetX() .. "," .. plot:GetY() .. ": " .. (ok and "ok" or ("failed: " .. tostring(err)))
		.. "; " .. before .. " -> " .. ImprovementName(plot) .. ".");
end
local IMPROVEMENTS = {
	{ "DevCEPg_O_I_Farm", "IMPROVEMENT_FARM" }, { "DevCEPg_O_I_Mine", "IMPROVEMENT_MINE" }, { "DevCEPg_O_I_Quarry", "IMPROVEMENT_QUARRY" },
	{ "DevCEPg_O_I_Pasture", "IMPROVEMENT_PASTURE" }, { "DevCEPg_O_I_Camp", "IMPROVEMENT_CAMP" },
};
for _, r in ipairs(IMPROVEMENTS) do
	local name = r[2];
	GameEvents[r[1]].Add(function(owner, unitID) SetImprovement(owner, unitID, name); end);
end
GameEvents.DevCEPg_O_I_Off.Add(function(owner, unitID) SetImprovement(owner, unitID, nil); end);

print("[DevCEPg][OBJ] ready.");
