-- Dev CE building gates (read by the Dev CE GameCore when the database loads).
--
-- A row makes the building or wonder buildable only in a city whose property Property is >= 1. Several rows on one building
-- must all hold. Set the property on cities with MODIFIER_SINGLE_CITY_ADJUST_PROPERTY (Key = Property, Amount), or with a
-- DynamicModifier of EFFECT_ADJUST_CITY_PROPERTY on COLLECTION_PLAYER_CITIES; give the modifier requirements (resource access,
-- a policy, ...) and the amount is taken off again when they stop holding.
-- While the gate is shut the building leaves the production queue but keeps its progress (a wonder keeps its partly built model),
-- is listed greyed out with Description (a LOC key; default LOC_DEVCE_BUILDING_GATE_SHUT) as the reason, and cannot be queued,
-- bought or granted for free.

CREATE TABLE IF NOT EXISTS DevCE_BuildingGates (
	BuildingType TEXT NOT NULL,
	Property TEXT NOT NULL,
	Description TEXT,
	PRIMARY KEY (BuildingType, Property),
	FOREIGN KEY (BuildingType) REFERENCES Buildings(BuildingType) ON DELETE CASCADE ON UPDATE CASCADE
);
