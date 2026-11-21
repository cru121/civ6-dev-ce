-- Dev CE city-centre specialists. Needs the Dev CE GameCore; without it these tables do nothing.
--
-- GlobalParameter DEVCE_CITY_CENTER_SPECIALISTS: 0 (default) = the game's normal specialists; 1 = specialists in the city centre:
--   each city has slots per specialist type, and a citizen in a slot gives that type's yields. The game's auto-assign (growth, city focus,
--   governors, AI) fills the slots together with tiles. Read when the database loads; change it only between games.
--
-- City properties (<SpecialistType> as in DevCE_Specialists):
--   DEVCE_SPECIALIST_SLOTS_<SpecialistType>    slots; set them with EFFECT_ADJUST_CITY_PROPERTY (Key, Amount)
--   DEVCE_SPECIALISTS_<SpecialistType>         working specialists, locked and auto (Dev CE writes it; read only for mods)
--   DEVCE_SPECIALISTS_LOCKED_<SpecialistType>  the player's lock request; applied at the next reallocation of the city's citizens
--   DEVCE_SPECIALIST_YIELD_<SpecialistType>_<YieldType>  extra yield per specialist of that type in this city, on top of
--                                              DevCE_Specialist_Yields; set it with EFFECT_ADJUST_CITY_PROPERTY (base yields only)

CREATE TABLE IF NOT EXISTS DevCE_Specialists (
	SpecialistType TEXT    NOT NULL,
	Name           TEXT,                        -- LOC key, for the UI
	Icon           TEXT,                        -- icon name, for the UI
	Color          TEXT,                        -- tint of its slots in the citizen lens: a UI colour name (COLOR_YIELD_SCIENCE) or "R,G,B"
	SortIndex      INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (SpecialistType)
);

-- What one citizen in a slot of this type gives.
CREATE TABLE IF NOT EXISTS DevCE_Specialist_Yields (
	SpecialistType TEXT    NOT NULL,
	YieldType      TEXT    NOT NULL,
	YieldChange    INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (SpecialistType, YieldType),
	FOREIGN KEY (SpecialistType) REFERENCES DevCE_Specialists (SpecialistType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (YieldType) REFERENCES Yields (YieldType) ON DELETE CASCADE ON UPDATE CASCADE
);

-- Great person points per turn from one citizen in a slot of this type (as District_CitizenGreatPersonPoints). They count in the
-- city's great person points, so city and player great person point modifiers apply to them.
CREATE TABLE IF NOT EXISTS DevCE_Specialist_GreatPersonPoints (
	SpecialistType       TEXT    NOT NULL,
	GreatPersonClassType TEXT    NOT NULL,
	PointsPerTurn        INTEGER NOT NULL DEFAULT 0,
	PRIMARY KEY (SpecialistType, GreatPersonClassType),
	FOREIGN KEY (SpecialistType) REFERENCES DevCE_Specialists (SpecialistType) ON DELETE CASCADE ON UPDATE CASCADE,
	FOREIGN KEY (GreatPersonClassType) REFERENCES GreatPersonClasses (GreatPersonClassType) ON DELETE CASCADE ON UPDATE CASCADE
);

INSERT OR IGNORE INTO GlobalParameters (Name, Value) VALUES ('DEVCE_CITY_CENTER_SPECIALISTS', 0);
