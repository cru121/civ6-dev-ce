-- Better National Parks: park rules as data. Without any other mod, the values below are what the Dev CE GameCore receives at game start.
-- Other mods can change them with their own SQL (list this mod as a dependency so the tables exist first).
CREATE TABLE IF NOT EXISTS NationalParkTypes (
	ParkType            TEXT    NOT NULL,
	MinTiles            INTEGER NOT NULL DEFAULT 4,      -- custom picker / DesignateCustom callers (1..32)
	MaxTiles            INTEGER NOT NULL DEFAULT 4,
	MinAppeal           INTEGER NOT NULL DEFAULT 2,      -- a tile needs appeal in [MinAppeal, MaxAppeal] unless an exemption below applies
	MaxAppeal           INTEGER NOT NULL DEFAULT 1000000,
	AllowImprovements   BOOLEAN NOT NULL DEFAULT 0,      -- 1 = tiles with an improvement may be part of a park
	AllowMountains      BOOLEAN NOT NULL DEFAULT 1,      -- 1 = mountain tiles fit whatever their appeal
	AllowNaturalWonders BOOLEAN NOT NULL DEFAULT 1,      -- 1 = natural wonder tiles fit whatever their appeal
	PRIMARY KEY (ParkType)
);

-- A tile with one of these districts may be part of a park; an allowed district is an explicit permission, so the appeal rule is skipped for it.
CREATE TABLE IF NOT EXISTS NationalPark_ValidDistricts (
	ParkType     TEXT NOT NULL,
	DistrictType TEXT NOT NULL,
	PRIMARY KEY (ParkType, DistrictType),
	FOREIGN KEY (ParkType) REFERENCES NationalParkTypes (ParkType) ON DELETE CASCADE ON UPDATE CASCADE
);

-- A tile holding one of these world wonders may be part of a park (decided by the wonder alone; DISTRICT_WONDER is not needed in the table above).
CREATE TABLE IF NOT EXISTS NationalPark_ValidWonders (
	ParkType     TEXT NOT NULL,
	BuildingType TEXT NOT NULL,
	PRIMARY KEY (ParkType, BuildingType),
	FOREIGN KEY (ParkType) REFERENCES NationalParkTypes (ParkType) ON DELETE CASCADE ON UPDATE CASCADE
);

-- Defaults of this mod (to be replaced by setup-screen options): 3..5 tiles for the custom picker, Preserve and a few landmark wonders allowed.
INSERT OR IGNORE INTO NationalParkTypes (ParkType, MinTiles, MaxTiles) VALUES ('NATIONAL_PARK_DEFAULT', 3, 5);

INSERT OR IGNORE INTO NationalPark_ValidDistricts (ParkType, DistrictType) VALUES ('NATIONAL_PARK_DEFAULT', 'DISTRICT_PRESERVE');

-- plain names (no SELECT from Buildings): wonders of content that loads after this file still count; unknown names are skipped by the Lua script
INSERT OR IGNORE INTO NationalPark_ValidWonders (ParkType, BuildingType) VALUES
	('NATIONAL_PARK_DEFAULT', 'BUILDING_STONEHENGE'), ('NATIONAL_PARK_DEFAULT', 'BUILDING_HANGING_GARDENS'), ('NATIONAL_PARK_DEFAULT', 'BUILDING_TEMPLE_ARTEMIS'),
	('NATIONAL_PARK_DEFAULT', 'BUILDING_MACHU_PICCHU'), ('NATIONAL_PARK_DEFAULT', 'BUILDING_MAHABODHI_TEMPLE'), ('NATIONAL_PARK_DEFAULT', 'BUILDING_CHICHEN_ITZA');
