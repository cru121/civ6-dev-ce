-- Dev CE tech and civic gates: tech and civic tooltips list the gates that are shut for the player (all gates when there is no
-- player). Loaded by ToolTipHelper's include("ToolTipLoader_", true).

local m_kGates = nil;   -- [itemType] = { { Property, Description }, ... }

local function LoadGates()
	m_kGates = {};
	local function add(tableName, column)
		local t = GameInfo[tableName];
		if t == nil then return; end
		for row in t() do
			local item = row[column];
			m_kGates[item] = m_kGates[item] or {};
			table.insert(m_kGates[item], { Property = row.Property, Description = row.Description });
		end
	end
	add("DevCE_TechnologyGates", "TechnologyType");
	add("DevCE_CivicGates", "CivicType");
end

local function GateLines(typeName, playerId)
	if m_kGates == nil then LoadGates(); end
	local gates = m_kGates[typeName];
	if gates == nil then return nil; end
	local pPlayer = (playerId ~= nil and playerId >= 0) and Players[playerId] or nil;
	local lines = {};
	for _, gate in ipairs(gates) do
		local shut = true;
		if pPlayer ~= nil then
			local value = pPlayer:GetProperty(gate.Property);
			shut = (type(value) ~= "number") or value < 1;
		end
		if shut then
			table.insert(lines, Locale.Lookup(gate.Description or "LOC_DEVCE_GATE_SHUT"));
		end
	end
	if #lines == 0 then return nil; end
	return Locale.Lookup("LOC_DEVCE_GATE_HEADER") .. "[NEWLINE]" .. table.concat(lines, "[NEWLINE]");
end

local function Wrap(kind)
	local base = g_ToolTipGenerators[kind];
	if base == nil then return; end
	g_ToolTipGenerators[kind] = function(typeName, playerId, bBaseValues)
		local tooltip = base(typeName, playerId, bBaseValues);
		local extra = GateLines(typeName, playerId);
		if extra == nil then return tooltip; end
		if tooltip == nil or tooltip == "" then return extra; end
		return tooltip .. "[NEWLINE][NEWLINE]" .. extra;
	end
end

Wrap("KIND_TECH");
Wrap("KIND_CIVIC");
